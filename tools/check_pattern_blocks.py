#!/usr/bin/env python3
"""Per-block transport assertion for PatternTest captures.

PatternTest deliberately mixes DC constant blocks with a 440 Hz sine, so the
whole-file spur/level checks in analyze_capture.py cannot pass on it by design.
What PatternTest is actually for is block alignment: the constant blocks must
arrive byte-exact and in order, and the sine must land at exactly 440 Hz. That is
what this checks.

    tools/check_pattern_blocks.py <capture.raw> [label]
"""

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 controllercustom@myyahoo.com
# Part of Mozzi-tinyusb. See LICENSE at the repository root.

import sys

import numpy as np

SR = 48000


def longest_run(mask):
    """Return (length, start, end_exclusive) of the longest contiguous True run."""
    best = (0, -1, -1)
    i = 0
    n = len(mask)
    while i < n:
        if mask[i]:
            j = i
            while j < n and mask[j]:
                j += 1
            if j - i > best[0]:
                best = (j - i, i, j)
            i = j
        else:
            i += 1
    return best


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    label = sys.argv[2] if len(sys.argv) > 2 else path
    sr = SR
    raw = np.fromfile(path, dtype='<i2').astype(np.int64)

    # The 0x5A5A block is the value 23130 held constant for a whole second.
    a5_len, a5_start, a5_end = longest_run(raw == 0x5A5A)
    if a5_len == 0:
        print(f"{label}: FAIL - no 0x5A5A constant block found")
        return 1
    block_exact = bool((raw[a5_start:a5_end] == 0x5A5A).all())

    # Everything between that block and the trailing silence is the sine region.
    nz = np.flatnonzero(raw != 0)
    sine_a, sine_b = int(a5_end), int(nz[-1]) + 1
    seg = raw[sine_a:sine_b].astype(np.float64)
    trim = int(0.25 * sr)
    core = seg[trim:-trim] if len(seg) > 3 * trim else seg

    rms = float(np.sqrt(np.mean(core ** 2))) if len(core) else float('nan')
    n_fft = 1 << 16
    spec = np.abs(np.fft.rfft(core[:n_fft] * np.hanning(n_fft)))
    freq = np.fft.rfftfreq(n_fft, 1 / sr)
    peak = float(freq[np.argmax(spec)]) if len(core) >= n_fft else float('nan')
    clipped = int((np.abs(core) >= 32767).sum())
    tail_zero = bool((raw[sine_b:] == 0).all())

    checks = [
        (f"0x5A5A block    {a5_len} samples constant={block_exact}", block_exact),
        (f"sine rms        {rms:9.1f} (expect {16384 / 2 ** 0.5:.1f})",
         abs(rms - 16384 / 2 ** 0.5) < 5),
        (f"sine pitch      {peak:9.2f} Hz (expect 440)", abs(peak - 440) < 1),
        (f"DC mean         {core.mean():9.1f}", abs(core.mean()) < 20),
        (f"clipped         {clipped} samples", clipped == 0),
        (f"trailing zeros  {tail_zero} ({len(raw) / sr - sine_b / sr:.2f}s of silence)",
         tail_zero),
    ]
    ok = all(c[1] for c in checks)
    print(f"{label}:")
    for line, good in checks:
        print(f"  [{'ok' if good else 'XX'}] {line}")
    print(f"  => {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

def longest_run(mask):
    """Return (start, end_exclusive) of the longest contiguous True run."""
    best = (0, -1, -1)
    i = 0
    n = len(mask)
    while i < n:
        if mask[i]:
            j = i
            while j < n and mask[j]:
                j += 1
            if j - i > best[0]:
                best = (j - i, i, j)
            i = j
        else:
            i += 1
    return best[1], best[2]

# The 0x5A5A block is the value 23130 held constant for a whole second.
a5_len, a5_start, a5_end = longest_run(raw == 0x5A5A)
if a5_len == 0:
    print(f"{label}: FAIL - no 0x5A5A constant block found")
    sys.exit(1)
block_exact = bool((raw[a5_start:a5_end] == 0x5A5A).all())

# Everything up to the first zero of trailing silence is the sine region.
nz = np.flatnonzero(raw != 0)
sine_a, sine_b = int(a5_end), int(nz[-1]) + 1
seg = raw[sine_a:sine_b].astype(np.float64)
trim = int(0.25 * sr)
core = seg[trim:-trim] if len(seg) > 3 * trim else seg

rms = float(np.sqrt(np.mean(core ** 2))) if len(core) else float('nan')
N = 1 << 16
spec = np.abs(np.fft.rfft(core[:N] * np.hanning(N)))
freq = np.fft.rfftfreq(N, 1 / sr)
peak = float(freq[np.argmax(spec)]) if len(core) >= N else float('nan')
tail_zero = bool((raw[sine_b:] == 0).all())

checks = [
    (f"0x5A5A block    {a5_len} samples constant={block_exact}", block_exact),
    (f"sine rms        {rms:9.1f} (expect {16384/2**0.5:.1f})",
     abs(rms - 16384 / 2 ** 0.5) < 5),
    (f"sine pitch      {peak:9.2f} Hz (expect 440)", abs(peak - 440) < 1),
    (f"DC mean         {core.mean():9.1f}", abs(core.mean()) < 20),
    (f"clipped         {int((np.abs(core) >= 32767).sum())} samples",
     int((np.abs(core) >= 32767).sum()) == 0),
    (f"trailing zeros  {tail_zero} ({len(raw)/sr - sine_b/sr:.2f}s of silence)",
     tail_zero),
]
ok = all(c[1] for c in checks)
print(f"{label}:")
for line, good in checks:
    print(f"  [{'ok' if good else 'XX'}] {line}")
print(f"  => {'PASS' if ok else 'FAIL'}")
sys.exit(0 if ok else 1)
