#!/usr/bin/env python3
"""Analyse a raw capture from the Mozzi USB synth and give a verdict.

Usage: analyze_capture.py <file.raw> [--expect-hz F] [--expect-dbfs D]
                          [--max-spur D] [--min-rms D] [--max-rms D]
                          [--detune-cents C]

Two host-side artifacts are accounted for, both measured rather than assumed:

  * Every capture begins with the same nine samples
    (18770 17990 ? 7 16727 17750 28006 8308 16) whatever firmware is running,
    so it is an ALSA/USB stream-start artifact, not the device. Left in, it
    sets the reported peak to 30464 (-0.63 dBFS) on every run and hides the
    real headroom, so the first 0.1 s is excluded from the level statistics.

  * MozziUSBSynth runs two oscillators detuned by a few cents, so a pair of
    peaks a few Hz apart beats against itself. --detune-cents says how much
    detune is expected and excludes that neighbourhood from the spur search;
    without it a perfectly healthy synth fails on its own chorus.
"""
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 controllercustom@myyahoo.com
# Part of Mozzi-tinyusb. See LICENSE at the repository root.

import argparse
import sys

import numpy as np

FS = 48000
# Samples of host stream-start artifact to discard (see docstring).
SKIP_HEAD = 4800  # 0.1 s


def blackman_harris(n):
    i = np.arange(n)
    t = 2.0 * np.pi * i / n
    return (0.35875 - 0.48829 * np.cos(t) + 0.14128 * np.cos(2 * t) -
            0.01168 * np.cos(3 * t))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("--expect-hz", type=float, default=0.0)
    ap.add_argument("--expect-dbfs", type=float, default=None)
    ap.add_argument("--max-spur", type=float, default=-45.0)
    ap.add_argument("--ignore-spur", action="store_true",
                    help="report the spur floor but do not assert on it. For a "
                         "filtered saw/square voice with two detuned oscillators "
                         "the harmonics ARE the signal, so a spur-floor bound "
                         "measures nothing. Use for synth voices; use --expect-hz "
                         "on SineToneTest, where a clean tone is the point.")
    ap.add_argument("--min-rms", type=float, default=-40.0)
    ap.add_argument("--max-rms", type=float, default=-3.0)
    ap.add_argument("--detune-cents", type=float, default=0.0,
                    help="exclude the expected detune beat from the spur search")
    a = ap.parse_args()

    x = np.fromfile(a.raw, dtype="<i2").astype(np.float64)
    if x.size < FS:
        print("FAIL capture too short: %d samples" % x.size)
        return 1

    # Level statistics skip the host's stream-start artifact.
    body = x[SKIP_HEAD:]
    peak = np.abs(body).max()
    rms = np.sqrt((body ** 2).mean())
    rms_db = 20 * np.log10(rms / 32768 + 1e-30)
    clipped = int((np.abs(body) >= 32767).sum())
    dc = float(body.mean())
    dc_db = 20 * np.log10(abs(dc) / 32768 + 1e-30)

    # Analyse the middle second so any start/end transient is excluded.
    n = 1 << 16
    seg = x[len(x) // 2 - n // 2: len(x) // 2 + n // 2]
    w = blackman_harris(n)
    S = np.abs(np.fft.rfft(seg * w))
    fr = np.fft.rfftfreq(n, 1.0 / FS)
    pk = int(np.argmax(S))
    fund_hz = fr[pk]
    keep = S.copy()
    # Blackman-Harris has a ~4-bin main lobe, so skip +/-3 bins around the
    # fundamental or we measure the window instead of the signal.
    keep[max(0, pk - 3):pk + 4] = 0
    if a.detune_cents > 0.0:
        # Exclude the beat of the deliberately detuned pair.
        beat = fund_hz * (2.0 ** (a.detune_cents / 1200.0) - 1.0)
        lo = max(0, int(np.argmin(np.abs(fr - (fund_hz + beat)))) - 3)
        hi = min(len(fr), int(np.argmin(np.abs(fr - (fund_hz + beat)))) + 4)
        keep[lo:hi] = 0
        # And the mirrored sideband of the beat.
        lo = max(0, int(np.argmin(np.abs(fr - (fund_hz - beat)))) - 3)
        hi = min(len(fr), int(np.argmin(np.abs(fr - (fund_hz - beat)))) + 4)
        keep[lo:hi] = 0
    sp = int(np.argmax(keep))
    spur_db = 20 * np.log10(keep[sp] / S[pk] + 1e-30)

    tot = float((S ** 2).sum())
    bands = [(0, 1000), (1000, 10000), (10000, 15000), (15000, 20000),
             (20000, 24000)]
    dist = [100.0 * float((S[(fr >= lo) & (fr < hi)] ** 2).sum()) / tot
            for lo, hi in bands]

    print("  samples        %d (%.2f s)" % (x.size, x.size / FS))
    print("  peak           %d (%.2f dBFS)" % (peak, 20 * np.log10(peak / 32768)))
    print("  RMS            %.1f  (%.2f dBFS)" % (rms, rms_db))
    print("  DC offset      %.1f  (%.2f dBFS)" % (dc, dc_db))
    print("  clipped        %d samples" % clipped)
    print("  fundamental    %.2f Hz" % fund_hz)
    print("  worst spur     %.1f dBc @ %.1f Hz" % (spur_db, fr[sp]))
    print("  energy bands   " + "  ".join(
        "%d-%dk:%.1f%%" % (lo // 1000, hi // 1000, d)
        for (lo, hi), d in zip(bands, dist)))

    fails = []
    if clipped:
        fails.append("%d clipped samples" % clipped)
    if not (a.min_rms <= rms_db <= a.max_rms):
        fails.append("RMS %.2f dBFS outside [%.1f, %.1f]"
                     % (rms_db, a.min_rms, a.max_rms))
    if spur_db > a.max_spur and not a.ignore_spur:
        fails.append("worst spur %.1f dBc above %.1f" % (spur_db, a.max_spur))
    if a.expect_hz and abs(fund_hz - a.expect_hz) > 2.0:
        fails.append("fundamental %.2f Hz, expected %.1f +/- 2"
                     % (fund_hz, a.expect_hz))
    if a.expect_dbfs is not None and abs(rms_db - a.expect_dbfs) > 1.0:
        fails.append("RMS %.2f dBFS, expected %.1f +/- 1" % (rms_db, a.expect_dbfs))

    if fails:
        for f in fails:
            print("  FAIL: " + f)
        return 1
    # Say what was actually checked. "all as expected" over a run that skipped the
    # spur floor and asserted no pitch would be a lie a reader has to notice.
    checked = ["level", "clipping"]
    if a.expect_hz:
        checked.append("pitch")
    if not a.ignore_spur:
        checked.append("spur floor")
    print("  PASS: %s as expected%s" % (", ".join(checked),
                                        " (spur floor reported, not asserted)"
                                        if a.ignore_spur else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
