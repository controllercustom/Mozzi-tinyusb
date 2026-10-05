# DSP notes

## Why the rate conversion exists

`MOZZI_AUDIO_RATE` must be a power of two — Mozzi's oscillators and filters
size their state from it. No legal USB audio rate is: 48000, 44100, 96000 are
all 2^n × 3 × 5^k or 2^n × 3^2. So Mozzi runs at 32768 and `Uac2Bridge`
converts 32768 -> 48000 (ratio 375/256, a 1.465 gain).

Conversion is unavoidable rather than optional: a host capture device must
deliver exactly 48000 samples per second, and `tud_audio_write()` hands the
endpoint whatever the sketch produced, one 98-byte frame per millisecond.

## Why two stages and not one

The naive answer is a single polyphase stage with `L = 375` branches (the
numerator of 375/256). That does not work here.

In a polyphase interpolator/decimator, the filter's stopband has to suppress
aliases spaced at the decimation rate, and the *passband* of each branch
replica lands only `fs_out` away. With one 375-branch stage the passband
replicas are spaced 48000/125 = 384 Hz apart (the branch count is 375, but the
branch rate is the intermediate rate 49152/128 = 384 Hz), and the replicas'
own skirts sit inside the band we are trying to pass. Measured with
`tools/sweep_design.sh`, a single-stage attempt has its branch response pinned
near 0 dB all the way out to 20–24 kHz by those replicas: it is not a filter
with poor stopband, it is not a filter at all in that region.

Splitting the ratio fixes it:

| Stage | Ratio | Rate in -> out | Branches | Taps |
|---|---|---|---|---|
| A | ×3/2 | 32768 -> 49152 | 3 | 22 |
| B | ×125/128 | 49152 -> 48000 | 125 | 22 |

Each stage now has its branch rate far above the passband edge, so the replicas
are outside the band of interest and the stopband is genuine. The rule this
follows, recorded in `tools/gen_polyphase.py`:

* **branches = the up factor `L`**, and the branch selected for output `n` is
  `(M·n) mod L`;
* **the tap array is reversed** relative to the usual convolution order, because
  the decimation phase selects the *oldest* sample as tap 0.

## Measured response

From `tools/sweep_design.sh` and `tests/test_resampler.cpp`:

| Frequency | Level |
|---|---|
| 0 – 10 kHz | 0.0 dB |
| 12 kHz | −1.1 dB |
| 17 – 23 kHz (image band) | −80 … −89 dB |

Worst spur across the test sweep: **−68.4 dBc** (at 201 Hz). Offline functional
test: **48 checks, 0 failures** — pitch exactness, stopband, no dropped or
duplicated samples, determinism (two runs bit-identical), and the underrun
accounting.

End-to-end on hardware the conversion is transparent to measurement: a
440.000 Hz tone generated at 32768 Hz arrives at **440.19 Hz** with a 2nd
harmonic at −183 dBc (numerically zero) and an RMS of 6466.7 against a
predicted 6466.

## FIFO sizing, and why `tud_audio_write()` is not back-pressure

The audio EP IN FIFO is configured `overwritable = true`
(`src/class/audio/audio_device.c`). Writing into a full FIFO therefore
*silently overwrites the oldest audio* rather than returning a short count, so
it cannot be used to pace the synth. `Uac2Bridge` exposes
`Uac2Bridge::kUsbEpBytes` (98) so the sketch can gate on occupancy:

```cpp
while (tu_fifo_count(ff) <= (uint16_t)(ff->depth - Uac2Bridge::kUsbEpBytes)) {
  bridge.fill(chunk, kChunkSamples);
  tud_audio_write(chunk, sizeof(chunk));
}
```

Slack defaults to 4 ms of audio (4 × 98 B = 4 frames = 196 samples at 48 kHz).
Deeper is not better: the FIFO is also where TinyUSB's clock-deviation
compensation looks to decide between 94/96/98-byte packets, and a very deep FIFO
shifts that decision.

## Buffer sizes

| Buffer | Size | Holds | Why |
|---|---|---|---|
| synth ring | 256 samples | 7.8 ms at 32768 Hz | absorbs a control-rate gap without an underrun |
| intermediate ring | 128 samples | 2.6 ms at 49152 Hz | topped up to 72 before each `fill()` |
| USB EP IN FIFO | 392 B (4 × 98) | 4 ms | TinyUSB default; 4× rather than 8× because of the note above |

`fill()` returns the number of underruns it had to substitute silence for. The
sketch reports them rather than swallowing them, because an underrun is the
only way this design produces audible glitches and it should be visible.

## Pacing, restated

Every stage is demand-driven, and the reason is not tidiness — it is
correctness. `loop()` runs ~400,000 times a second against ~1,000 USB frames a
second, so anything that generates "one frame per iteration" is generating
400× too much audio and the endpoint ships a strided subsample of it. The host
then hears a pitch determined by CPU speed, and a waveform with a discontinuity
at every strided frame boundary. Quantified in AGENTS.md: a generated 440 Hz
tone arrives as 493.5 Hz, and as 440.000 Hz once gated on endpoint room.

The same reasoning applies to `tud_arduino_task()`. On the pico OSAL it calls
`tud_task_ext(UINT32_MAX)`, and a UAC2 isochronous IN endpoint is only polled by
the host while something is capturing — so a synth that parks there stops making
sound entirely, which is exactly what was observed
(`bridge.produced()` frozen at 784 samples while the host recorded garbage). The
sketches call `tud_task_ext(0, false)` instead: drain what is pending, return,
and let the audio path stay self-clocking.

## Level staging

Mozzi's `Oscil` is 8-bit and `StateVariable` works in that domain, so the voice
has ~7 bits to start with. Chaining three amplitude scalings as
`(v * x) >> 8` therefore costs about 48 dB and puts the whole voice near
−48 dBFS RMS — inaudible, and with no clipping to hint that anything is wrong.
`MozziUSBSynth` promotes to a 32-bit accumulator once (`v * 256`) before the
vibrato, envelope and level stages, giving −11.3 dBFS RMS with about 7 dB of
headroom.

A `DCfilter` after the lowpass strips the filter integrators' DC offset, which
otherwise showed as a −22.9 dBc spur at 0 Hz.
