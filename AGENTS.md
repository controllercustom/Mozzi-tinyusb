# AGENTS.md — development history and lessons learned

Working notes for this repository. `README.md` is for people who want to *use* the
sketches; this file is for whoever changes them next. Everything here was
measured, not assumed, and the measurement is given so it can be re-checked
rather than taken on faith.

Read this before touching the audio path, the test harness, or the build.

---

## The three faults that cost the most, because none of them look like faults

The dangerous class of bug in this project: the device enumerates, the ALSA card
appears, and `arecord` fills a file with something recognisably audio that is
wrong. Nothing reports a failure.

### 1. Generate audio at the endpoint's rate, not the CPU's

The obvious structure — fill a frame, `tud_audio_write()` it, repeat — is wrong,
and so is the one in TinyUSB_Arduino's own `Device_Audio` example. `loop()` runs
~400,000 times a second against ~1,000 USB frames a second, so the frames the
endpoint actually transmits are a *strided subsample* of a far faster oscillator.
The host hears a pitch set by the CPU speed, with a discontinuity at every
strided frame boundary.

Measured on an RP2040, generating one 440 Hz frame per `loop()`:

```
raw @90k: ... -12288, 5953, 5065, 4160 ...     <- waveform jumps mid-stream
FFT peak 493.50 Hz   (should be 440)           <- +12.16%
```

Gating generation on room in the endpoint FIFO, same build:

```
FFT peak 440.000 Hz  3rd harmonic -97.9 dBc    <- 0.0000% error
```

**Rule: every stage is demand-driven.** `audioHook()` runs only while the synth
ring has room; `bridge.fill()` runs only while the EP FIFO has room for a whole
frame. Never hand over a partial frame.

`tud_audio_write()` **cannot** be used for back-pressure at all. TinyUSB marks the
audio EP IN FIFO `overwritable`, so writing into a full FIFO silently overwrites
the oldest audio instead of returning a short count. Size the write from
`tu_fifo_count(ff)` against `ff->depth` instead.

### 2. `Oscil`'s no-arg constructor leaves the table pointer uninitialised

```cpp
Oscil<2048, MOZZI_AUDIO_RATE> osc;                 // table uninitialised
Oscil<2048, MOZZI_AUDIO_RATE> osc(SIN2048_DATA);   // correct
```

`readTable()` then dereferences whatever the pointer happened to hold. It does
not crash, and it does not look like garbage samples — it looks like a harsh buzz
with the fundamental *missing* and 8 kHz dominant, which reads as a DSP problem
rather than an uninitialised pointer. Always pass the table to the constructor.

### 3. Mozzi's async ADC hangs the RP2040

`MOZZI_ANALOG_READ_STANDARD` makes `startMozzi()` call `setupMozziADC()`, whose
`dma_claim_unused_channel()` never returns on this board: `setup()` stops after
its banner and `loop()` never runs. A/B on the same board:
`ANALOG_READ_STANDARD` hangs, `ANALOG_READ_NONE` runs.

At a 256 Hz control rate there is nothing to gain from claiming a DMA channel and
a shared `DMA_IRQ_0` handler, so `updateControl()` uses plain `analogRead()`.

---

## Build and dependency decisions, and why

### The TinyUSB library is passed with an explicit `--library`, because the failure is silent

This bit us. `arduino-cli` resolves a library by scanning for whoever provides the
included header. This project used to pass **no** `--library` for the TinyUSB
stack and picked up the sketchbook's `Arduino-TinyUSB` symlink.

When two libraries both ship `ArduinoTinyUSB.h`, `arduino-cli` takes the first in
directory order **with no warning**. With both installed it compiled
`Arduino-TinyUSB` — including its `bsp_r4.cpp` — and never looked at the pinned
one, producing a plausible-looking binary from the wrong stack.

Verified experimentally:

* both installed, no `--library` → `libraries/Arduino-TinyUSB/.../bsp_r4.cpp.o`
  present, `TinyUSB_Arduino` never compiled;
* explicit `--library libs/TinyUSB_Arduino` against a sketchbook that still held
  the old symlink → `TinyUSB_Arduino` compiled, no `bsp_r4.cpp.o`.

So `build.sh` passes it explicitly, runs inside an isolated environment
(`scripts/avenv.sh`) whose libraries dir starts **empty**, and after every build
asserts the result: the build dir must contain `libraries/TinyUSB_Arduino` and
must **not** contain the old library's `bsp_r4.cpp.o`. The guard was tested
against a deliberately sabotaged build dir rather than assumed to pass.

### The per-sketch config needs an explicit define (CLI) or a copy (IDE)

Same class of problem, opposite direction. `src/tusb_option.h` includes
`"tusb_config.h"`, which resolves against the **library's own `src/`** first — and
`TinyUSB_Arduino` ships a `src/tusb_config.h` shim. So the sketch's config is not
found automatically and the unified config is used instead.

Measured on `SineToneTest`, at `TINYUSB_SHA=f8927bc`:

| Build | Flash | `CFG_TUSB_DEBUG` |
|---|---|---|
| No flags (IDE default) | 77340 B | 0 |
| `-DCFG_TUSB_DEBUG=0` only | 77340 B | 0 |
| With `-DARDUINO_TINYUSB_CONFIG_FILE` | 63980 B | 0 |

The explicit define is still required, but **no longer for `CFG_TUSB_DEBUG`**. The
reason rows 1 and 2 differ from row 3 is the per-sketch feature set alone, not the
debug level and not the FIFO: the whole 13360 B is classes switched off.

**The FIFO is not part of that difference, despite what this file used to say.** It
claimed the sketch config "sizes the audio EP IN FIFO (4 → 8 frames)". It does not.
Each sketch config sets `CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ` to
`(4 * CFG_TUD_AUDIO_EP_SZ_IN)` — bit-for-bit what `tusb_config_common.h:121` and the
unified config `tusb_config_arduinotinyusb.h:165` already set. Checked at all three
pins this project has used (`c246c15`, `f8927bc`, `186129d`): always `4x`, never `8x`,
so there has never been an 8-frame audio FIFO to lose. The `#undef`/`#define` pair is
therefore a no-op today.

`4x` is deliberate and worth keeping — TinyUSB reads this FIFO's occupancy to choose
between its 94/96/98-byte clock-deviation packets, so a deeper buffer skews that
decision. Measured steady at 196/392 while capturing. The `#undef`/`#define` is also
worth keeping as insurance against `tusb_config_common.h` changing under a future
bump. What was wrong is the prose: `SineToneTest/tusb_config_arduinotinyusb.h` carried a header
saying the FIFO was "Raised to 8 x 98 B = 8 ms" directly above code setting `4x`, and
"8 ms of slack" was written twice in three lines. A no-op that reads as load-bearing
is worse than obviously dead code, because the next person will not touch it.

`CFG_TUSB_DEBUG` used to be the dangerous one, and the numbers here were:

| Build | Flash | `CFG_TUSB_DEBUG` |
|---|---|---|
| No flags (IDE default) | 87308 B | **2** |
| `-DCFG_TUSB_DEBUG=0` only | 77340 B | 0 |
| With `-DARDUINO_TINYUSB_CONFIG_FILE` | 63980 B | 0 |

Logging runs from inside the USB ISR, where a 40-character line at 115200 baud
blocks ~3.5 ms, longer than a whole 1 ms audio frame. Confirmed with a
preprocessor probe, not inferred from the size.

Upstream `f8927bc` ("Library-wide `CFG_TUSB_DEBUG` default lowered from 2 to 0")
fixed this at the source, so row 1 dropped 87308 → 77340 B. The three defences
here are unchanged and still all load-bearing: `-DCFG_TUSB_DEBUG=0` in
`build.sh`, `#define CFG_TUSB_DEBUG 0` in each sketch config, and the
`-DARDUINO_TINYUSB_CONFIG_FILE` define that makes those two reachable at all.
Verified by probe both ways — `f8927bc` reports `0` with no flags, `c246c15`
reported `2`.

**Do not delete the redundant guards because the default improved.** The pin is
a SHA of a repo its own author calls experimental; a future bump can lower this
default again, and a sketch config that silently inherits `2` fails as audio
corruption rather than as an error. The 9968 B of ISR logging is exactly the
kind of fault this file exists to catch.

Verified across the bump: all 8 sketch/board flash sizes are byte-identical
(`SineToneTest` pico/pico2 63980/61728, `MozziUSBSynth` pico/pico2/zero
67348/66056/40812, `PatternTest` pico/pico2 55728/54252, `MozziGigaSynth` giga
145664), and `./scripts/build.sh --full` builds all 19 sketch/board pairs with
the library assertion still holding.

Equal flash size is weak evidence on its own, so it was checked harder. On
`SineToneTest`/pico, `.text`, `.rodata` and `.data` are **byte-identical**
between the two pins, and `nm -S --defined-only` gives an identical symbol
table — all 919 symbols at matching addresses and sizes. So the firmware really
is the same program and the recorded 440.19 Hz / −14.10 dBFS results carry over
without re-running the hardware test.

The `.elf` files are nonetheless *not* byte-identical (~37 kB of `.debug_info`
differs). Cause: the commit added comment blocks, so `tusb_config_common.h` went
164 → 174 lines and `tusb_config_arduinotinyusb.h` 196 → 205, and every
translation unit including them gets shifted DWARF line numbers. Debug metadata
only; no code moved, which the matching symbol addresses confirm.

**Trap when comparing builds this way:** `objcopy --only-section=.debug_info`
silently emits **0 bytes**, so hashing it compares empty against empty and every
section looks "identical". That is a false pass — it was caught by checking the
extraction length against the section header's size (`.debug_info` is 648530 B,
extraction was 0). Use `objcopy --dump-section <name>=<file>`, and sanity-check
that a section's extracted length is non-zero before believing a hash match.

This is the same class of trap as the build-path note above, one level deeper:
that one is about comparing builds from different directories, this one is about
the comparison reporting success while measuring nothing.

### Mozzi is pinned to a fork, and may stop being

`MOZZI_OUTPUT_EXTERNAL_CUSTOM` is the demand-driven mode the whole project is
built on. `libs/Mozzi/internal/config_checks_mbed.h:74` advertises it as
supported, but `MozziGuts_impl_MBED.hpp` has branches only for `EXTERNAL_TIMED`,
`INTERNAL_DAC` and `PDM_VIA_SERIAL`. Nothing defines `MozziPrivate::startAudio()`
or `stopMozzi()`, so `MozziGuts.hpp:280` and `:305` fail:

```
error: 'startAudio' was not declared in this scope
error: 'stopMozzi' is not a member of 'MozziPrivate'
```

Upstream bug, present in `7f1e5b4`, in current master, and in the published
`Mozzi@2.0.4` archive. 2.0.4 is the latest release, so
`arduino-cli lib install Mozzi` cannot resolve it — verified by installing it and
reproducing the identical failure. The pin is therefore
[controllercustom/Mozzi](https://github.com/controllercustom/Mozzi) at `e392692a`,
whose single commit over upstream is that one-file fix.

This replaced an earlier local `patches/*.patch` plus an `apply_patches()` step in
`setup_libs.sh`. The patch mechanism existed only because `libs/` is gitignored,
so a hand edit would be silently discarded on the next refetch. That whole layer
is gone now; if upstream accepts the fix, point `MOZZI_REPO` back at
`sensorium/Mozzi` and bump the SHA.

**When writing that branch, it must go inside the existing output-mode chain**, as
an `#elif` beside `EXTERNAL_TIMED`. As a standalone block it would define
`startAudio()`/`stopMozzi()` a second time and collide whenever another mode is
selected.

### Uno R4 was removed rather than left in the table

`TinyUSB_Arduino` drops `renesas_uno` entirely. `r4` is absent from the FQBN and
define tables on purpose: building it fails *inside the library* with
`'TU_EDPT_STATE_RX_PENDING' undeclared` in `device/usbd.c`, not the library's own
`Unsupported board` `#error`, because the `renesas_uno` core ships its own TinyUSB
headers that win the `-I` search. A board listed in the table that cannot build is
worse than an absent one. The dead `#if defined(ARDUINO_TINYUSB_BOARD_R4)`
descriptor branch was deleted from all three `usb_descriptors.cpp`; it called
`arduino_tinyusb_custom_*_desc()`, which no longer exists.

### The per-sketch specialised configs upstream

`186129d` ("Give sketches the ability to create optimized tusb_config parameters")
is the third commit from initial. It adds `src/config/tusb_config_union.h`, replaces
each example's `tusb_config.h` with a `tusb_config_arduinotinyusb.h`, and in
`ArduinoTinyUSB.h` moves `#include "tusb.h"` up above the API declarations and wraps
those declarations in `#if CFG_TUD_ENABLED` / `#if CFG_TUH_ENABLED`. The intent is
that a single-role config cannot link against a role it does not compile in, turning
what used to be a silent empty function body into a compile error at the call.

The guards are a real behaviour change for us in principle, so check the role macros
rather than assuming: the three device-only sketches set `CFG_TUD_ENABLED 1` /
`CFG_TUH_ENABLED 0` and `MozziGigaSynth` sets both to 1, all explicitly. So
`tud_arduino_init`/`tud_arduino_task` are declared where they are used, `tuh_*` only
in the GIGA sketch, and `arduino_tinyusb_probe` is role-agnostic throughout.

Verified unaffected, rather than assumed:

* `src/tusb_option.h` **unchanged**, so `-DARDUINO_TINYUSB_CONFIG_FILE` still selects
  a config directly. This is the load-bearing part for this project.
* `src/tusb_config.h` unchanged; `src/tusb_config_arduinotinyusb.h` is the *same blob*
  (`101f757`) as at `f8927bc`.
* `src/config/tusb_config_common.h` is comment-only, so every config **value** —
  including the `CFG_TUSB_DEBUG` default this file cares so much about — is unchanged.
* The vendored TinyUSB tree is untouched: nothing under `src/device`, `src/class`,
  `src/host` or `src/portable` differs. Still `b80f1c10` per the new `src/VERSION`.

Verification, following the same standard as the `f8927bc` bump above:

* `--full` builds 19/19, library assertion still holding.
* All 19 flash sizes unchanged, and `text`/`rodata`/`data`/symbol-count identical to
  the `f8927bc` build for every pair.
* On `SineToneTest`/pico, `.text` (53084 B), `.rodata` (10640 B) and `.data` (4968 B)
  are byte-identical by SHA-256, and `nm -S --defined-only` gives an identical
  919-symbol table. Same program, so the recorded 440.19 Hz / −14.10 dBFS results
  carry over with no hardware re-test. Same 919 as the bump above, which is the
  point: the firmware did not move.
* The `README.md` flag table reproduces exactly at this pin: no flags 77340 B,
  `-DCFG_TUSB_DEBUG=0` only 77340 B, with the define 63980 B.

**Trap when counting symbols: use the cross `nm`, not the host one.** Counting
`SineToneTest`/pico with host `nm -S --defined-only` gives 1943, with
`arm-none-eabi-nm` gives 919 — the host tool reports ARM mapping symbols and
section entries the cross tool does not. It does not corrupt a same-tool A/B
comparison, which is how it survived a first pass here unnoticed, but it does make
two counts incomparable, and a count that cannot be reconciled with an earlier
recorded one is the signal that the wrong tool was used.

**One upstream bug at this pin, since fixed one commit later.** At `186129d` the
`src/VERSION` and the union header's own comment both state that
`src/tusb_config_arduinotinyusb.h` is "one line: it includes this file". It is not —
it is the standalone 205-line config, and `config/tusb_config_union.h` (224 lines)
is included by nothing at all. The union is duplicate content that nothing reads,
so the two can drift, and anyone who follows the documented
`cp src/config/tusb_config_union.h src/tusb_config_arduinotinyusb.h` copies a file
that is not the source of truth.

**Fixed upstream at `c6b84e2` ("Fix duplicate configs"), one commit past our pin.**
That commit is `-201/+53` across exactly those two files: the active config drops
249 → 52 lines and becomes the one-line `#include "config/tusb_config_union.h"`
its own documentation always claimed. Verified against the public repo, not taken
on trust. At `c6b84e2` the union is the single source of truth and the documented
undo is correct again.

So: **the pin is one commit behind a real upstream fix**, and this is the reason to
bump it. Until then, nothing here depends on the union — every build passes
`-DARDUINO_TINYUSB_CONFIG_FILE`, and the IDE path restores from its own backup
rather than the union, so the drift costs nothing except a caveat in the docs.

### The sketch config is named `tusb_config_arduinotinyusb.h`, and that is not our choice

Each sketch's config used to be `tusb_config.h`, and `build.sh` reached it with
`-DARDUINO_TINYUSB_CONFIG_FILE`. That made the Arduino IDE path **unbuildable**,
because the IDE has no UI for extra build flags — so the one audience the IDE
section was written for could not follow it. Worth being blunt about that: the
instruction was not merely awkward, it was impossible, and it sat in the README
through several rounds of review.

`186129d` is what makes the IDE path possible. The library now reads its
**active config slot** at `src/tusb_config_arduinotinyusb.h`, and
`TinyUSB_Arduino`'s documented way to use your own is a *file copy* over that
slot, not a define. Its README gives the reason it cannot be an `#include`: the
Arduino build never puts your sketch folder on the include path for library
sources, and including it from the `.ino` would change only the sketch's
translation unit, leaving the library's out of sync.

So the filename is forced: the file has to be called `tusb_config_arduinotinyusb.h`
because that is the name of the slot it must overwrite. A thin wrapper does not
work — once copied into `src/`, a quoted `#include "tusb_config.h"` resolves
against the library's own directory and picks up the library's shim instead of
the sketch's. Renamed all four, and `build.sh`'s `-D` now points at the new
name. One file, two consumers.

Verified inert before believing it: baseline of all 19 sketch/board pairs
(`.text` and `.bin` sha256) captured first, then renamed, then `--full` re-run.
All 19 identical. The rename cannot change codegen; that is not an argument, a
measurement.

The IDE path is then verified **equivalent**, not merely plausible: four
libraries installed in a scratch sketchbook, `Uac2Bridge` from the extracted
release zip, config installed by `scripts/ide_setup.sh`, then compiled with
`--fqbn` and a sketch path and *nothing else* — no `-D`, no `--library`:

```
IDE      275e2796ece7032537bd7b481d36582b   134456 B
build.sh 275e2796ece7032537bd7b481d36582b   134456 B
```

Byte-identical. So the two paths are the same program. The `9b00f58` commit
message records `...36bd...`: a one-digit transcription error. A deterministic
rebuild on 2026-10-05 matches this hash at 35 of the 36 recorded hex digits —
different files never agree that closely, so the binary was always `37bd` and
the record was wrong. `-DARDUINO_TINYUSB_BOARD_GIGA`
turned out to be unnecessary — the library's `board_auto.h` detects the board
from core macros — so `build.sh` passes it only to stay independent of include
order, which is what its comment always claimed.

Cost of getting it wrong, measured: with the library's own config instead,
`SineToneTest` is 147824 B on GIGA and 77348 B on Pico, against 130136 / 63980.
17.7 kB on GIGA, and **the build still succeeds** — nothing warns.

### The union and the active config have drifted — at this pin, and now fixed

Documented above as a possibility; at `186129d` it is a fact:

```
src/config/tusb_config_union.h   224 lines   included by nothing
src/tusb_config_arduinotinyusb.h 205 lines   what the stack actually reads
```

So at this pin the library's documented undo —
`cp src/config/tusb_config_union.h src/tusb_config_arduinotinyusb.h` — is **not
a restore**. It replaces the shipped config with different, drifted content.
`scripts/ide_setup.sh` backs the library's own bytes up on first install and
restores those instead; verified by `diff -r` against a fresh copy coming back
empty.

**`c6b84e2`, one commit past our pin, is "Fix duplicate configs"** — `-201/+53`
across exactly those two files, making the active config the one-line
`#include "config/tusb_config_union.h"` the docs always described (249 → 52 lines).
At that commit the union is the single source of truth and the documented undo is
correct again.

**Consequence: bumping the pin to `c6b84e2` is the fix**, and it would let this
section and the corresponding README paragraph be deleted rather than caveated.
Not done here, because a pin bump is a deliberate act that owes the same
verification as any other: baseline, bump, `--full`, all 19 pairs byte-identical.
The backup-based `--restore` is correct either way, so there is no rush.

### Two traps the build assertion cannot catch, so `ide_setup.sh` does

`build.sh` passes `--library` explicitly and then asserts which library was
compiled. The IDE can do neither, so these two are only reachable from the IDE
path and are checked by `scripts/ide_setup.sh` instead:

* **`Arduino-TinyUSB` shadows `TinyUSB_Arduino`.** Both ship `ArduinoTinyUSB.h`,
  and arduino-cli resolves it by directory order with no warning.
  `Arduino-TinyUSB` sorts first, so the old stack wins. Confirmed by the build
  directory listing `libraries/Arduino-TinyUSB`. Measured nuance: the local
  `~/Arduino-TinyUSB` is a *later* revision of this same project, and for GIGA the
  two gave a byte-identical `.bin` despite differing in 200 files — because
  `class/audio/audio_device.c`'s difference is an
  `#if !defined(ARDUINO_TINYUSB_BOARD_R4)` guard that is inactive on GIGA, and
  `bsp_giga.cpp`'s is a comment. So "it built fine" is not evidence the right
  library was used, and "identical size" is not evidence either.
* **Mozzi fork vs upstream cannot be told by version.** Both say `2.0.4`.
  `ide_setup.sh` greps `internal/MozziGuts_impl_MBED.hpp` for
  `MOZZI_OUTPUT_EXTERNAL_CUSTOM`, which is exactly what the fork adds.

### Builds are reproducible; binaries are not

Verified from a pristine copy of the tree with no `libs/` at all:
`setup_libs.sh` fetches all three pins and both GIGA sketches and the Pico build
succeed. Flash sizes are identical run to run.

Binaries are **not** byte-identical across checkout directories, and that is
expected: the RP2040 core embeds `__FILE__` paths. A build at a path 4 characters
longer is 8 bytes larger (2 embedded paths). Not a reproducibility problem in the
source — do not chase it.

### The FIFO is 4 ms everywhere, including in prose

The FIFO is `4 * CFG_TUD_AUDIO_EP_SZ_IN` = 392 B = 4 frames = ~4.08 ms at 48 kHz.
That is true at every pin this project has used, so the "8x / 8 ms" language was
never describing anything real.

The dead `#undef`/`#define` pair in each sketch config is kept as
insurance against `tusb_config_common.h` changing, and its comments say `4x`. It
took a second pass to catch that `docs/dsp-notes.md` still said **8 ms** in two
places — "Slack defaults to 8 ms of audio", and the buffer table's `392 B
(4 × 98) | 8 ms` row, which is self-contradictory on its face — plus
`MozziUSBSynth.ino`'s "~8 ms of synth ring + 8 ms of USB FIFO". All corrected;
the synth ring's 7.8 ms is real and stays.

The lesson is the one already written above: a number that reads as
load-bearing but was never true is worse than obviously dead code, and the grep
that finds it is `grep -rn "8 *ms" --include=*.md --include=*.ino`, not a
reviewer's eye.

### Mozzi's `#warning` about `audioOutput()` is a false positive

Every build of the three Mozzi sketches prints:

```
internal/config_checks_generic.h:148: warning: #warning "Mozzi is configured to
use an external void 'audioOutput(const AudioOutput f)' function. Please define
one in your sketch"
```

`config_checks_generic.h:147` fires on `MOZZI_OUTPUT_EXTERNAL_TIMED` **or**
`MOZZI_OUTPUT_EXTERNAL_CUSTOM`, and the preprocessor cannot know whether the
sketch defines the function, so it cannot be satisfied or silenced. All three
sketches *do* define it at global scope, as the guard at `MozziGuts.hpp:318`
requires. Do not
"fix" this by adding anything, and do not let it be mistaken for a real fault —
it is now documented under *Expected build warnings* in the README.

`MozziGuts_impl_RP2040.hpp:298` ("Automatic random seeding is not implemented")
is also benign: nothing here uses `random()`.

### `scripts/avenv.sh` is vendored and deliberately unlicensed here

Copied from the local `arduino-virtual-env` skill, which carries **no licence
statement**. It therefore has a `PROVENANCE` header and no SPDX tag, and is in the
`EXCLUDE` set of `tools/add_license_headers.py` so a future run cannot stamp MIT
over it. It is a build-isolation helper only; `NO_ISOLATE=1 ./scripts/build.sh`
bypasses it. If the author confirms a licence, add it there.

---

### `Uac2Bridge` was extracted into its own first-party repo

It used to be `lib/Uac2Bridge/` here. It is now
[controllercustom/Uac2Bridge](https://github.com/controllercustom/Uac2Bridge),
pinned here by SHA like any other dependency. **First-party, not third-party:**
same MIT terms and same copyright holder as this repo's `LICENSE`.

The reason is that the problem it solves was never Mozzi's. A synth engine
wanting a power-of-two audio rate and a USB audio endpoint wanting 48 kHz cannot
both be satisfied, because no legal USB audio rate is a power of two. That is a
constraint of USB audio on a microcontroller, and anyone hitting it needs the
converter regardless of which synth they use.

The DSP tests and filter-design tooling moved with it — `test_resampler.cpp`,
`probe_response.cpp`, `sim_bridge.py`, `gen_polyphase.py`, `sweep_design.sh`.
**Deliberately delegated, not forked.** `scripts/run_tests.sh` now runs the pinned
library's own `run_tests.sh` rather than keeping a copy of its tests, so there is
one copy of each test and bumping the pin picks up new ones. Dropping the tests
here instead would have left CI checking nothing DSP-shaped.

**There is no packaging script, and that was verified rather than assumed.** It
looked like one would be needed — the IDE's "Add .ZIP library" wants a particular
archive shape — but GitHub's own zip already *is* that shape: one top-level
directory holding `library.properties` and `src/`. Installed the unmodified
`git archive` output into a scratch sketchbook and built a sketch against it:
byte-identical firmware, and `tests/*.cpp` is not compiled despite containing
`main()`, because arduino-cli builds only `src/`. So distribution is Code ▸
Download ZIP, or Releases ▸ Source code (zip) on a tag — and the tag is worth
having, since it names the archive `Uac2Bridge-1.0.0.zip` instead of
`Uac2Bridge-main.zip`.

A packaging tool would also have been a liability: the library repo root *is* the
library, so a deny-list would have swept `tests/`, `tools/` and the CI workflow
into a release archive. GitHub's zip sidesteps the question instead of answering
it.

Behaviour is unchanged; this moved code. Verified the standard way: a baseline of
all 19 sketch/board pairs (`.text` and `.bin` sha256) was captured before the
extraction, `--full` re-run after, and the two compared. A move cannot change
codegen — that is not the argument, the comparison is.

Until the library repo is pushed, build against the local checkout:

```bash
UAC2BRIDGE_REPO=~/Uac2Bridge ./scripts/setup_libs.sh
```

**Validated 2026-10-05, end to end, since the repo is not pushed yet**
(`git ls-remote` on the GitHub URL fails). `setup_libs.sh` cloned from the path
and landed at pinned `a76bcb9`; `--full` built 19/19 with the library assertion
holding in every pair; `run_tests.sh` passed (48 resampler checks, `sim_bridge`
sweep, licence sweep). `SineToneTest`/giga's `.bin` came back byte-identical to
the baseline above, and a repeat build bit-identical to itself: every core was
installed 2026-09-04 to 09-12 (before the baseline was recorded) and
arduino-cli dates from 2026-07-23, so the toolchain and all pins are the ones
the baseline was measured under. The extraction was also diffed directly:
`lib/Uac2Bridge` at `c72305b^` against the pinned clone differs only in
comments and `library.properties` — the code is the same.

Local `main` sits six commits ahead of the pin, one of them behavioural
(`a652b23`: `push()` drops a sample when the ring is full instead of
overwriting the oldest, which is what out-of-order output would have been).
It passes the library's own suite, so it is a candidate pin; bumping to it
owes the standard baseline/bump/`--full` comparison, not a re-derivation.

## GIGA-specific faults

### The console must be opened before the USB stack, or the board HardFaults

`SineToneTest` HardFaulted on its first GIGA flash. Resolving the fault address
rather than guessing:

```
PC 0x08047E6E -> mbed::SerialBase::writeable()
LR 0x080439B7 -> arduino::UART::write()  cores/arduino/Serial.cpp:235
```

`tud_arduino_init()` claims OTG_FS, and a Mbed UART touched after that faults.

The fix lives behind `#if defined(ARDUINO_ARCH_MBED)`. **The RP2040 ordering is
deliberately left byte-for-byte unchanged** — it is the ordering the verified
440.19 Hz result was measured under, and tidying it into a uniform shape would
produce a change nobody could re-measure. On the RP2040, `Serial1` is a plain
hardware UART that TinyUSB never touches, so either order works; that is why the
bug never appeared there.

### Three OpenOCD traps, all hit on this board

* **`interface/stlink.cfg` is a different driver depending on the build.** In the
  system OpenOCD tree it is `hla` (legacy) and fails with `Unsupported transport`
  or `BUG: current_target out of bounds`. `interface/stlink-dap.cfg` is the native
  `st-link` driver and is the one to use, with `transport select dapdirect_swd` —
  **not** `swd`, which is the `hla` name. Forcing a `dapdirect_*` transport onto
  `hla`, or vice versa, fails *after* the deprecation spam and reads like a driver
  bug.
* **Do not mix trees.** A bare `-f interface/stlink.cfg` from the xpack binary
  silently falls back to `/usr/share/openocd/scripts`. Pass `-s` from the same tree
  the binary came from.
* **`adapter speed` is a ceiling, not a request.** The V3 snaps to a fixed ladder;
  1000 kHz was used here and worked; 100 kHz was rejected and rounded to 50 kHz.

### The console is not the probe

The STLINK-V3 presents its own VCP as a `ttyACM` node. The GIGA's `Serial1` is on
a CP210x as `ttyUSB`. Reading the probe's VCP looks like it works and yields
nothing. `hw_test.sh` resolves the GIGA console by the CP210x's VID:PID, not by
`ID_SERIAL` (which is the generic `CP2102_USB_to_UART_Bridge_Controller_0001` and
can differ per adapter).

### Arduino_AdvancedAnalog

Mozzi's mbed port references it. `build.sh` installs it only when `giga` is
selected rather than adding a dependency to every build.

---

## Test-harness traps

### `hw_test.sh` and multiple boards

* **Probe↔board wiring is not discoverable in software.** The script finds the
  right probe by asking each one whether it can see the chip we expect —
  `target/rp2040.cfg` only initialises against an RP2040. Guessing flashed the
  wrong board and produced a plausible-looking result for the wrong chip.
* **The capture card must be chosen by USB port, never by enumeration order.** With
  several boards on identical firmware, "card 1" is not stable. The script refuses
  to guess and prints the candidates; pass `CARD=<n>`.
* **Card indices move when a flash changes the PID.** Re-enumerating
  `MozziGigaSynth` (which presents `2341:5002`, deliberately distinct from the
  `5001` of the other sketches) renumbered everything. So the card is re-resolved
  *after* flashing, and the script says so when the index moved.
* **Establishing the mapping:** do not assume the probe and its board share a hub
  port. Flash each board and watch which USB port's `devnum` changes. That is how
  the table in `README.md` was derived.

### Waiting for a device after a flash

A flash resets the board and the kernel needs seconds to cycle it. From `dmesg`
on the GIGA: `usb disconnect` → `new full-speed USB device` was 3.6 s, then
another 5.1 s → `New USB device found`. About **8.7 s** before it is really there,
during which the card index can shift. The card also appears in `/proc/asound`
before `snd-usb-audio` has created its PCM, and `arecord` then fails with
`audio open error: No such file or directory` against firmware that is fine. The
old fixed `sleep 3` recorded a card that was not there yet.

So `hw_test.sh` retries the actual capture rather than sleeping a fixed amount.

**Trap:** `arecord --dump-hw-params` is **not** usable as a readiness probe. It
prints the hardware parameters and then exits 1 whether or not the device is
healthy, so testing its exit status fails on a perfectly good card. This was
caught by running the probe against a known-good card before trusting it. A failed
capture open costs ~0.1 s, so retrying the real capture is both correct and cheap.

### `PatternTest` was asserting something impossible

It ran `analyze_capture.py`, but whole-file spur and level checks **cannot** pass
on a capture that deliberately mixes DC blocks with a sine: the RMS is a blend of
both and the "worst spur" is just the DC blocks' own energy. It reported FAIL on
good firmware on all three boards.

Replaced with `tools/check_pattern_blocks.py`, which asserts per block and
**anchors on the `0x5A5A` block** because block phase is arbitrary relative to the
start of a capture. Getting that anchoring wrong looks like a failing test too —
it cost a round of false alarms. Measured on all three boards:

```
0x5A5A block    byte-exact          sine pitch   440.19 Hz
sine rms        11585.1-11585.4     (16384/sqrt2 = 11585.2)
```

### Don't assert what is not defined

`MozziGigaSynth`'s capture is asserted on level, clipping and in-band energy only.
Its pitch comes from the nanoKONTROL2 CC cache, which starts at built-in defaults
and is whatever the controller last sent — so no frequency is defined. And its spur
floor is not a property of the sketch: a filtered saw+square pair with two
detuned oscillators has strong harmonics by construction, measured −8.9 dBc.
Bounding either would assert nothing, so `--ignore-spur` was added to
`analyze_capture.py` and the PASS line now names what was actually checked rather
than claiming "level, pitch, spur floor and clipping" on a run that skipped two of
those. Use `SineToneTest` for a fixed-pitch assertion.

### OpenOCD: `program` needs `init` before it and `reset run` after

* `-f rp2350.cfg` does **not** resolve — it must be `-f target/rp2350.cfg`. A bare
  name resolves, then fails deep inside with
  `embedded:startup.tcl:72: Error: Can't find rp2350.cfg`, which reads like a
  chip-detection problem rather than a path one.
* `program` must be preceded by an explicit `-c "init"` and followed by
  `reset run` in the same invocation. Letting `program` do its own init failed on
  the RP2350 with `Can't find rp2350` when a previous openocd instance had left
  the target halted. And `program` alone leaves the board halted with a stale USB
  device, so `arecord` fails to open a card that is actually fine.
* Arm the console **before** the flash: the flash's `reset run` is what makes the
  board print, and the banner lands within a second of it.
* Stop the console reader on exit, or it keeps claiming the tty and interleaves
  with the next run's reader.

### Pico 2 (RP2350): two flash workarounds and a one-shot PatternTest

* `program <elf>` cannot flash the Pico 2 builds. The 4 MB-layout RP2350 ELF
  carries three overlapping tail-marker `PT_LOAD`s at `0x10013550`, and OpenOCD's
  ELF writer aborts with `Section at 0x10013550 overlaps section ending at
  0x10013564` / `Flash write aborted`. Flash the `.bin` instead, raw at the bank
  origin: `flash write_image erase <sketch>.ino.bin 0x10000000`. The `.bin` is
  the flat image from `0x10000000` (boot2 included), so this is exactly what the
  ELF writer would have laid down minus the marker segments it chokes on.
* Two more prerequisites, both measured rather than guessed. `rp2350.cfg` leaves
  `FLASHSIZE` at 0, which sends the rp2xxx driver down QSPI size-detection; pass
  `set FLASHSIZE 0x200000` *before* `-f target/rp2350.cfg` to suppress it (log
  then reports `QSPI Flash size override = 2048 KiB in 512 sectors`). And `init`
  leaves the target in `TARGET_RESET`, so the erase is rejected with `Target not
  halted` (`rp2xxx.c:968`) surfacing as `failed erasing sectors 0 to 19` — an
  explicit `reset halt` after `init` fixes it. Working sequence: `init` →
  `reset halt` → `flash write_image erase` → `verify_image` → `reset run`.
  Verified with `-d`: without the halt the erase fails before any ROM call.
* `PatternTest` is one-shot per boot and its block counter only advances while
  the endpoint is mounted (`if (!tud_audio_mounted()) return`), so the `0x5A5A`
  block sits ~3–4 s after the host first opens the stream and the tail is silence
  until the next reboot. A `sleep 4` before the capture misses it — measured
  1.911 s of sine tail plus 4.089 s of silence with no `0x5A5A` at all. Capture
  immediately after `reset run` with a tight poll (0.2 s, first openable card at
  the board's port); on FAIL, `reset run` restarts the pattern. Passing capture:
  49000 constant `0x5A5A` samples, sine rms 11585.4 (expect 11585.2), 440.19 Hz.

---

## Migration history

The USB stack was an unpublished local checkout (`~/Arduino-TinyUSB`, symlinked
into the sketchbook), so the build only worked on that machine. Moved to
[controllercustom/TinyUSB_Arduino](https://github.com/controllercustom/TinyUSB_Arduino).

It was close to a drop-in, and it is worth recording *why* so nobody re-derives
it: both libraries vendor the same upstream TinyUSB commit (`b80f1c10`), so 178 of
199 vendored files are byte-identical, and the two this project actually depends
on — `class/audio/*` and `portable/raspberrypi/rp2040/*` — are identical
outright. Verified by compiling every sketch unmodified.

That is also why the RP2040 results after the migration match the pre-migration
baseline *exactly*: 440.19 Hz, −14.10 dBFS, spurs −69.4 dBc, 0 clipped. The
evidence that the migration preserved behaviour is that the audio and RP2040
drivers did not change.

---

## Conventions worth keeping

* **`build.sh` asserts the library after every build.** A build can succeed while
  linking the wrong TinyUSB. Do not remove the check; extend it if the pin
  changes.
* **Checks that nothing invokes will rot.** `add_license_headers.py` existed with
  a `--check` mode and nothing ran it, so the MIT sweep had no enforcement. It is
  now called by `run_tests.sh`, which is what CI runs on every push, and the
  failure path was verified by stripping a header and confirming the non-zero
  exit — not just by watching it pass.
* **Keep board-conditional code narrowly guarded.** The GIGA console ordering sits
  behind `#if defined(ARDUINO_ARCH_MBED)` precisely so the verified RP2040 path
  stays untouched. Do not "tidy" conditional code into a single shape without
  re-measuring every branch it covers.
* **Record measurements, including the failures.** Several of the traps above are
  only distinguishable from a misconfiguration because the measurement is written
  down. A comment saying "this is fragile" helps nobody; the number does.
* **Verify the guard, not just the happy path.** The library assertion was tested
  against a sabotaged build dir; the readiness probe was tested against a
  known-good card. Both would have shipped as false confidence otherwise.
* **`setup_libs.sh` is idempotent** and reports each pin. Re-run it freely.
* **Offline tests need no hardware** (`./scripts/run_tests.sh`) — run them first;
  they catch DSP regressions in seconds.