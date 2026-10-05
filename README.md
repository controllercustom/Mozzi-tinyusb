# Mozzi-tinyusb

A Mozzi subtractive synth whose audio leaves the board over **USB Audio Class 2**
as a 48 kHz mono capture device, so it shows up on the host as an ordinary
microphone/line input that any DAW or `arecord` can capture.

```
Mozzi (32768 Hz, 16-bit)
  -> Uac2Bridge   32768 -> 48000, two-stage polyphase, 22 taps
  -> UAC2 EP 0x81 98 B frames, 1 per ms
  -> host capture device  (48 kHz, S16_LE, mono)
```

## The sketches

Open the folder you want and build it. Each is self-contained.

### `MozziUSBSynth` — the synth

One monophonic subtractive voice: two `Oscil` oscillators (summed, detunable), a
`StateVariable<LOWPASS>` filter, and a stepped ADSR, with a 0.7 Hz LFO giving
±25% vibrato.

| Pin | Control |
|---|---|
| A0 (GP26) | filter cutoff, 80 Hz – 12 kHz |
| A1 (GP27) | oscillator detune, 0 – 300 cents |
| A2 (GP28) | level |
| GP15 | pluck (button to ground; starts released) |

Two defines make it usable on a bare board with nothing wired up. A0–A2 float
when unconnected and give the voice no defined pitch or level, so without these
it is silent or arbitrary. Uncomment them at the top of the sketch — or pass
them as `-D` flags to the scripted build (`EXTRA_DEFINES="-DNO_POTS
-DNO_BUTTON"`), which is the same effect:

* `NO_POTS` — fixed settings (2 kHz cutoff, 7 cents, level 200) instead of
  reading A0..A2.
* `NO_BUTTON` — retrigger the envelope on every control tick instead of waiting
  for a button, so it makes sound as soon as it is plugged in.

### `MozziGigaSynth` — the GIGA sketch

The GIGA R1 has **two independent USB controllers**, so a single sketch can be a
USB device *and* a USB host at the same time. This one does both:

| | Port | Controller | Role |
|---|---|---|---|
| USB-C | OTG_FS, rhport 0 | device | UAC2 48 kHz mono capture → the host |
| USB-A | OTG_HS, rhport 1 | host | nanoKONTROL2 MIDI control in |

It is **GIGA-only**. Every other board here has a single native USB port that
TinyUSB owns exclusively, so it can be a device or a host but never both —
`scripts/build.sh` therefore skips this sketch elsewhere instead of failing.

Control mapping (nanoKONTROL2 in CC mode — hold **Set Marker + Cycle** while
plugging it in):

| Control | Maps to |
|---|---|
| Faders CC 0–7 | cutoff, resonance, attack, decay, sustain, release, LFO rate, volume |
| Knobs CC 16–23 | pitch, detune, osc mix, filter EG, LFO depth, drive, spare, vibrato |
| Solo 0–2 | octave −1 / 0 / +1 |
| Mute 0 | LFO on/off (latch) |
| Play / Stop | gate the envelope |
| Rec 0 | retrigger |

With no controller attached it uses built-in defaults and drones, so it makes
sound on its own.

Two things to know before using it:

* **Console is `Serial1` on the CP210x** (D0/D1). Mbed's USB CDC stops working
  once the device stack owns OTG_FS. Upload over USB by double-tapping RESET
  to enter the bootloader — an STLINK-V3 on SWD also works, but is optional.
* `Serial1.begin()` must come **before** any `tud_`/`tuh_` init in `setup()`. The
  sketch already does this; if you add your own init, put it after the console.

### `SineToneTest` — the reference tone

A single 440 Hz tone at −14.1 dBFS RMS, through the whole chain (Mozzi →
Uac2Bridge → UAC2 EP IN) with nothing else in the way. Built to have exactly one
correct answer, so it is the first thing to flash when checking a board.

### `PatternTest` — the transport diagnostic

Feeds a sequence of one-second blocks — silence, `+full-scale DC`, `+1 LSB`,
`0x5A5A`, then a 440 Hz sine — and logs each block to the console so one capture
can be matched block-for-block against the device's own log. It is the fastest
way to tell a transport problem from a signal problem:

* blocks do not line up, or the constant blocks are not constant and exact → the
  USB path is at fault;
* blocks line up but the sine's *pitch* is wrong → audio generation is not paced
  (a time-warped constant still looks perfect, which is why the sine is in the
  same run).

## Status

Verified on real hardware 2026-10-02 by recording with `arecord` and asserting on
the capture, not by assuming the device enumerated.

| Sketch | RP2040 | RP2350 | GIGA |
|---|---|---|---|
| `SineToneTest` | **verified** | **verified** | **verified** |
| `MozziUSBSynth` | **verified** | **verified** | **verified** |
| `MozziGigaSynth` | n/a | n/a | **verified** |
| `PatternTest` | **verified** | **verified** | **verified** |

| | `SineToneTest` | `MozziUSBSynth` |
|---|---|---|
| RP2040 | 440.19 Hz, −14.10 dBFS RMS, spurs −69.4 dBc, 0 clipped | 109.86 Hz, −11.30 dBFS RMS |
| RP2350 | 440.19 Hz, −14.10 dBFS RMS, spurs −69.4 dBc, 0 clipped | 109.86 Hz, −11.30 dBFS RMS |
| GIGA | 440.19 Hz, −14.09 dBFS RMS, spurs −69.4 dBc, 0 clipped | 109.86 Hz, −11.35 dBFS RMS |

The three chips are indistinguishable on the wire, which is the point: the
32768 → 48000 resampler is exact on all of them. Device-side telemetry agrees —
996–998 transfers/s at 97–98 B ≈ 48,800 samples/s, zero underruns.

`MozziGigaSynth` is asserted on level and clipping only. Its pitch comes from the
controller and is not defined without one, and its spur floor is not a meaningful
property — a filtered saw+square pair with two detuned oscillators has strong
harmonics by construction (−8.9 dBc measured). Use `SineToneTest` when you want a
fixed-pitch, spur-floor assertion.

## Building

### Command line

```bash
./scripts/setup_libs.sh          # fetch the 3 pinned libraries into libs/
./scripts/build.sh               # build MozziUSBSynth + SineToneTest for pico
./scripts/build.sh --full        # every sketch on every supported board
./scripts/build.sh MozziGigaSynth giga
./scripts/run_tests.sh           # offline DSP tests + licence check, no hardware
```

`run_tests.sh` needs only `g++` and `python3`, and is what CI runs on every
push. The firmware compile is not in the default CI path because it downloads a
board core; see [`.github/workflows/ci.yml`](.github/workflows/ci.yml).

### Arduino IDE

Three steps. Neither needs a compiler flag — the IDE has no UI for extra build
flags, so anything that requires `-D` is unavailable here by construction.

#### 1. Install the four libraries

`FixMath` comes from the Library Manager; the other three are not in it, so
they install from a zip: download from GitHub, then
**Sketch ▸ Include Library ▸ Add .ZIP library**.

| Library | How | Source |
|---|---|---|
| `FixMath` | Library Manager | Sketch ▸ Include Library ▸ Manage Libraries…, search `FixMath`, install version `1.0.9` |
| `TinyUSB_Arduino` | **zip** | [controllercustom/TinyUSB_Arduino](https://github.com/controllercustom/TinyUSB_Arduino) → Code ▸ Download ZIP |
| `Mozzi` | **zip** | [controllercustom/Mozzi](https://github.com/controllercustom/Mozzi) — the *fork*, see [GIGA builds](#giga-builds) |
| `Uac2Bridge` | **zip** | [controllercustom/Uac2Bridge](https://github.com/controllercustom/Uac2Bridge) → Code ▸ Download ZIP, or Releases ▸ Source code (zip) for a tag |

`Arduino_AdvancedAnalog` is needed for GIGA too, and *is* in the Library
Manager.

Use the Mozzi **fork**, not `sensorium/Mozzi`. Both report version `2.0.4`, so
the version number cannot tell them apart — but only the fork implements
`MOZZI_OUTPUT_EXTERNAL_CUSTOM` on mbed, which is the audio mode this project is
built on. Upstream fails with `'startAudio' was not declared in this scope`.

#### 2. Install the sketch's config

Every sketch carries its own TinyUSB config, `tusb_config_arduinotinyusb.h`,
and it has to be put where TinyUSB actually reads it: copy it over the
installed library's own file at
`libraries/TinyUSB_Arduino/src/tusb_config_arduinotinyusb.h` inside your
sketchbook folder, replacing it. Back up the shipped file first if you want to
restore it later.
your sketch folder on the include path for library sources, and including it
from the `.ino` would change only the sketch's translation unit, leaving the
library's out of sync.

This step is not optional. Skipped, the build still succeeds — against the
library's full config — and you give up **17.7 kB on GIGA**:

| Build | GIGA | Pico |
|---|---|---|
| Library's own config (step 2 skipped) | 147824 B | 77348 B |
| This sketch's config installed | **130136 B** | **63980 B** |

The saving is entirely feature set: audio only, with CDC, HID, MSC and the whole
host stack compiled out. You lose no FIFO headroom — the audio EP IN FIFO is
4 × 98 B either way. (Flash figures move by a few bytes with the checkout path
length, because the RP2040 core embeds `__FILE__`; the 8 B difference between
the 77340 quoted elsewhere and the 77348 measured here is that effect.)

The configs differ per sketch. Three are device-only; `MozziGigaSynth` also
enables the USB host role, being a MIDI host and a UAC2 device at once. Copy
the other sketch's config file over before building a different sketch —
forgetting produces a *link* error naming the missing `tuh_*` call.

#### 3. Select the board and upload

Under Tools ▸ Board select the board you are building for (for example the
Arduino GIGA R1 WiFi), pick its port under Tools ▸ Port, open the sketch's
`.ino`, and press Upload.

* GIGA: if no port shows up, double-tap RESET to enter the bootloader first.
* Pico / Pico 2: hold BOOTSEL while plugging the board in (or tap RESET while
  holding BOOTSEL), then upload — no debug probe needed.

### Expected build warnings

Two warnings appear on every build and are **not** misconfiguration. Neither is
suppressible from the sketch.

```
libs/Mozzi/internal/config_checks_generic.h:148: warning: #warning "Mozzi is
configured to use an external void 'audioOutput(const AudioOutput f)' function.
Please define one in your sketch"
```

This one is an upstream Mozzi false positive. `config_checks_generic.h:147`
fires unconditionally for `MOZZI_OUTPUT_EXTERNAL_TIMED` *or*
`MOZZI_OUTPUT_EXTERNAL_CUSTOM`, because the preprocessor has no way to know
whether the sketch actually defines the function. All three Mozzi sketches do
define it, at global scope, as the guard at `MozziGuts.hpp:318` requires:
`MozziUSBSynth.ino`, `SineToneTest.ino` and `MozziGigaSynth.ino` each have
`void audioOutput(const AudioOutput f)`. So there is nothing to add and nothing
to fix. `PatternTest` does not include Mozzi and never emits it.

```
libs/Mozzi/internal/MozziGuts_impl_RP2040.hpp:298: warning: #warning Automatic
random seeding is not implemented on this platform
```

Benign: it comes from `MozziRandPrivate::autoSeed()`, and no sketch here uses
`random()` or `MozziRand`. It appears only on the RP2040/RP2350 builds.

### Boards

| Board | FQBN | Status |
|---|---|---|
| Raspberry Pi Pico (RP2040) | `rp2040:rp2040:rpipico:usbstack=nousb,dbgport=Serial1,flash=2097152_0` | verified |
| Raspberry Pi Pico 2 (RP2350) | `rp2040:rp2040:rpipico2:arch=arm,usbstack=nousb,dbgport=Serial1,flash=4194304_0` | verified |
| Arduino GIGA R1 WiFi | `arduino:mbed_giga:giga` | verified |
| Pico W | `rp2040:rp2040:rpipicow:usbstack=nousb,dbgport=Serial1,flash=2097152_0` | builds |
| Zero / M0 Pro (SAMD21) | `arduino:samd:arduino_zero_edbg` / `arduino:samd:mzero_pro_bl_dbg` | builds |
| Nano R4 | — | **not supported** |

`usbstack=nousb` is required on the Picos: with TinyUSB owning the native
controller, the core's own USB stack must not claim it first.

**Nano R4 is not supported.** `TinyUSB_Arduino` drops `renesas_uno`
outright — no `ARDUINO_TINYUSB_BOARD_R4`, no `bsp_r4.cpp`. It is absent from
the board table on purpose: building for it fails inside the library with a
misleading error (`'TU_EDPT_STATE_RX_PENDING' undeclared` in `device/usbd.c`)
rather than its own `Unsupported board` message, because the `renesas_uno`
core ships TinyUSB headers that win the `-I` search.

#### GIGA builds

GIGA is mbed, and Mozzi's mbed port does not implement
`MOZZI_OUTPUT_EXTERNAL_CUSTOM` — the audio mode this project depends on — even
though its own config check advertises it. The build then fails with
`'startAudio' was not declared in this scope`. Upstream `sensorium/Mozzi` has this
bug in `7f1e5b4`, in current master, and in the published `2.0.4` archive, and
2.0.4 is the newest release, so installing from the Library Manager cannot fix
it. Install the [controllercustom fork](https://github.com/controllercustom/Mozzi)
instead, which carries that one-file fix. GIGA also needs
`Arduino_AdvancedAnalog`, which `build.sh` installs for you (IDE: from the
Library Manager, as above).

## Verifying on hardware

This section is for Linux command-line users only — IDE users can ignore it.

`scripts/hw_test.sh` flashes a sketch, records 6 s with `arecord`, and asserts on
the capture:

```bash
./scripts/hw_test.sh SineToneTest pico     # flash, record, assert
CARD=2 ./scripts/hw_test.sh SineToneTest pico
CARD=4 ./scripts/hw_test.sh SineToneTest pico2
CARD=3 ./scripts/hw_test.sh SineToneTest giga
```

With more than one board attached, pass `CARD=<n>` — the script refuses to guess
and prints the candidates. Card numbers move with enumeration order, so read the
printed list rather than trusting any table.

Wiring is optional hardware. On the Picos, a Debug Probe's UART on GP0/GP1/GND
gives the console and SWD flashes the board — but without a probe, hold BOOTSEL
while resetting to upload instead. On the GIGA, an STLINK-V3 on SWD is likewise
optional: double-tap RESET to enter the bootloader and upload over USB. For the
console there, use a CP210x on the board's RX/TX/GND — **not** a probe's own
VCP, which is a separate `ttyACM` device.

## Why 32768 and not 48000

`MOZZI_AUDIO_RATE` must be a power of two and no legal USB audio rate is, so
`Uac2Bridge` converts 32768 → 48000. See `docs/dsp-notes.md` for why that is a
two-stage ×3/2 then ×125/128 cascade and not a single 375-branch stage.

## Layout

```
MozziUSBSynth/      the synth
MozziGigaSynth/     GIGA-only: MIDI host + UAC2 device at once
SineToneTest/       440 Hz reference tone
PatternTest/        transport-only diagnostic
scripts/            setup_libs.sh, build.sh, run_tests.sh, hw_test.sh, avenv.sh,
                    ide_setup.sh
tools/              analysis and licence-header helpers
.github/workflows/  ci.yml — offline checks on every push, plus one firmware build
libs/               pinned dependencies, fetched by setup_libs.sh (gitignored)
docs/dsp-notes.md   why the resampler has the shape it has
```

`Uac2Bridge` — the rate converter every sketch's audio path runs through — used
to live in `lib/Uac2Bridge/` here. It is now its own project,
[controllercustom/Uac2Bridge](https://github.com/controllercustom/Uac2Bridge),
pinned here like any other dependency: the problem it solves (a power-of-two
synth rate meeting a non-power-of-two USB clock) is not specific to Mozzi, and
anyone writing USB audio on a microcontroller needs it. See
[Licence](#licence) for the first-party/third-party distinction that creates.

Pinned dependencies, fetched by `scripts/setup_libs.sh` rather than taken from
the sketchbook:

* Mozzi `e392692a` — the [controllercustom fork](https://github.com/controllercustom/Mozzi),
  which is upstream `7f1e5b4` plus the mbed `EXTERNAL_CUSTOM` fix
* FixMath `21f3f8d4` (v1.0.9)
* TinyUSB_Arduino `186129d` — pinned by SHA because the repo is untagged and
  self-described as experimental; bumping it is a deliberate act. Three commits
  from initial: `c246c15`, the `CFG_TUSB_DEBUG` 2 → 0 default fix, and
  per-sketch specialised TinyUSB configs.
* Uac2Bridge `e9d124f` — [controllercustom/Uac2Bridge](https://github.com/controllercustom/Uac2Bridge).
  **First-party, not third-party:** this code was extracted from this repo, so it
  is MIT like everything else here. It is pinned by SHA anyway, for the same
  reason TinyUSB_Arduino is — so a build is reproducible and the revision is
  recorded in `setup_libs.sh` rather than in someone's working tree. Until that
  repo is pushed, build against a local checkout with
  `UAC2BRIDGE_REPO=~/Uac2Bridge ./scripts/setup_libs.sh`.

## Acknowledgments

Built with [OpenCode](https://opencode.ai).

## Licence

First-party code is MIT — see [LICENSE](LICENSE). Each source file carries an
`SPDX-License-Identifier: MIT` tag, applied and checked by
`tools/add_license_headers.py`.

That does **not** cover the dependencies, which keep their own licences and are
fetched into the gitignored `libs/`:

| Dependency | Licence |
|---|---|
| [TinyUSB_Arduino](https://github.com/controllercustom/TinyUSB_Arduino) | MIT (bundles upstream TinyUSB, MIT, Ha Thach; and ChaN's FatFS, BSD-style, in one example) |
| [controllercustom/Mozzi](https://github.com/controllercustom/Mozzi) | **LGPL-2.1-or-later** (Tim Barrass and the Mozzi Team) |
| [tomcombriat/FixMath](https://github.com/tomcombriat/FixMath) | **LGPL-3.0** |
| [controllercustom/Uac2Bridge](https://github.com/controllercustom/Uac2Bridge) | **MIT** — first-party, extracted from this repo; same licence and holder as `LICENSE` here |

Mozzi and FixMath are compiled as separate libraries linked into the sketch, not
vendored into it — the arrangement LGPL is designed around. The combined work
stays MIT while those libraries remain under their own terms and remain yours to
modify and redistribute.

Uac2Bridge is in the table because it lives in `libs/` and is pinned like the
rest, but it is **not a third-party dependency**: same MIT terms, same copyright
holder, and no `__FILE__`-embedded path from it appears in this repo. It is
fetched separately so that other USB-audio authors can take the rate converter
without taking the sketches.

One exception to the MIT sweep: `scripts/avenv.sh` is vendored from a local skill
that carries no licence statement, so it has a `PROVENANCE` header and no SPDX
tag. It is a build-isolation helper only — `NO_ISOLATE=1 ./scripts/build.sh`
bypasses it. It is listed in that script's `EXCLUDE` set, so a future
`add_license_headers.py` run cannot stamp MIT over it.

That script's `--check` mode is run by `scripts/run_tests.sh`, so the MIT sweep
cannot rot unnoticed; CI runs the same script on every push.

## Development notes

`AGENTS.md` holds the development history and the lessons learned: the faults that
did not announce themselves as faults, the traps in `hw_test.sh` and OpenOCD, and
why the build is shaped the way it is. Start there before changing the audio
path or the test harness.