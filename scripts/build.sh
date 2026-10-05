#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 controllercustom@myyahoo.com
# Part of Mozzi-tinyusb. See LICENSE at the repository root.

# Build the Mozzi USB synth sketches.
#
#   ./scripts/build.sh                 # all sketches, board "pico"
#   ./scripts/build.sh --full          # all sketches, every board
#   ./scripts/build.sh MozziUSBSynth   # one sketch
#   ./scripts/build.sh SineToneTest pico2
#
# Flags, and why:
#   --library libs/TinyUSB_Arduino  REQUIRED. arduino-cli resolves a library by
#                               scanning for whoever provides the included header,
#                               and when two libraries both ship
#                               ArduinoTinyUSB.h it silently picks the first in
#                               directory order. With both this library and the
#                               old Arduino-TinyUSB in the sketchbook it picked
#                               Arduino-TinyUSB and never looked at this one, no
#                               warning, a plausible-looking binary. An explicit
#                               --library wins over the sketchbook, so the build
#                               is pinned to libs/ and is reproducible anywhere.
#   -DARDUINO_TINYUSB_BOARD_*  the library auto-detects from core macros, but
#                               being explicit keeps the build independent of
#                               include order.
#   -DARDUINO_TINYUSB_CONFIG_FILE="<sketch>/tusb_config_arduinotinyusb.h"
#                               REQUIRED, and the only route on the command line.
#                               The per-sketch config is not found
#                               automatically: src/tusb_option.h includes
#                               "tusb_config.h", which resolves relative to the
#                               library's own src/ first (the library ships a
#                               src/tusb_config.h shim). Without this define the
#                               library's own config is used instead - measured
#                               77348 B instead of 63980 B on SineToneTest/pico,
#                               147824 B instead of 130136 B on GIGA. Since
#                               f8927bc that config no longer costs debug logging
#                               either (its CFG_TUSB_DEBUG default is 0), so the
#                               whole difference is feature set: this project's
#                               config compiles audio only.
#
#                               The filename is the library's *active config
#                               slot*, not a name of our choosing, so that IDE
#                               users -- who cannot pass -D at all -- can have
#                               the same file copied over that slot instead.
#                               See scripts/ide_setup.sh.
#   -DCFG_TUSB_DEBUG=0        belt and braces; the sketch config also sets it.
#                               Redundant since f8927bc raised the library default
#                               to 0, kept deliberately: the pin is a SHA of a repo
#                               its author calls experimental, and CFG_TUSB_DEBUG=2
#                               fails as audio corruption rather than as an error.
#   --export-binaries         emits .uf2 for the BOOTSEL drop, which is how the
#                             hardware test flashes (no Debug Probe needed for
#                             device mode).
#
# Isolation: the compile runs inside an arduino virtual environment (avenv.sh)
# whose libraries dir starts EMPTY, so nothing can leak in from
# ~/Arduino/libraries. Set NO_ISOLATE=1 to build against the real sketchbook
# instead (useful when the IDE is your only way to install a board core).
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD
BUILD=$ROOT/build

declare -A FQBN=(
  [pico]="rp2040:rp2040:rpipico:usbstack=nousb,dbgport=Serial1,flash=2097152_0"
  [pico2]="rp2040:rp2040:rpipico2:arch=arm,usbstack=nousb,dbgport=Serial1,flash=4194304_0"
  [picow]="rp2040:rp2040:rpipicow:usbstack=nousb,dbgport=Serial1,flash=2097152_0"
  [zero]="arduino:samd:arduino_zero_edbg"
  [m0pro]="arduino:samd:mzero_pro_bl_dbg"
  [giga]="arduino:mbed_giga:giga"
)
declare -A DEFINE=(
  [pico]=ARDUINO_TINYUSB_BOARD_PICO
  [pico2]=ARDUINO_TINYUSB_BOARD_PICO
  [picow]=ARDUINO_TINYUSB_BOARD_PICO
  [zero]=ARDUINO_TINYUSB_BOARD_ZERO
  [m0pro]=ARDUINO_TINYUSB_BOARD_ZERO
  [giga]=ARDUINO_TINYUSB_BOARD_GIGA
)
# No Uno R4 / Nano R4 entry. TinyUSB_Arduino drops renesas_uno support outright:
# no ARDUINO_TINYUSB_BOARD_R4, no bsp_r4.cpp, and no scripts/patch_renesas_core.sh
# to install a patched core. Building for r4 fails deep in the library with a
# misleading error ('TU_EDPT_STATE_RX_PENDING' undeclared, in device/usbd.c)
# rather than its own "Unsupported board" #error, because the renesas_uno core
# ships its own TinyUSB headers that win the -I search. So it is left out of the
# table entirely instead of being listed and failing confusingly.

# PatternTest belongs here. It was absent for a while while this file and the
# docs both claimed --full built every sketch: --full was building 13 pairs, not
# the 14 AGENTS.md recorded, because this array never listed it. It builds
# everywhere (zero/m0pro/picow/giga all measured), so nothing restricts it.
# With it, --full builds 19 pairs: 3 sketches x 6 boards + MozziGigaSynth/giga.
SKETCHES=(MozziUSBSynth SineToneTest PatternTest MozziGigaSynth)
BOARDS=(pico)

# Sketches restricted to particular boards. A sketch listed here is skipped on
# every other board instead of failing there.
#   MozziGigaSynth runs TinyUSB as a device (UAC2 on OTG_FS/Type-C) *and* a host
#     (MIDI on OTG_HS/Type-A) at the same time. That needs the GIGA's two
#     independent USB controllers. Every other board here has a single native
#     port that TinyUSB owns exclusively, so it can be a device or a host but
#     never both -- and CFG_TUH_MIDI would have no rhport to attach to.
declare -A SKETCH_BOARDS=(
  [MozziGigaSynth]="giga"
)

if [[ "${1:-}" == "--full" ]]; then
  shift
  BOARDS=(pico pico2 picow zero m0pro giga)
elif [[ "${1:-}" == "--list" ]]; then
  echo "sketches: ${SKETCHES[*]}"
  for s in "${SKETCHES[@]}"; do
    [[ -n "${SKETCH_BOARDS[$s]:-}" ]] && echo "  $s: only on ${SKETCH_BOARDS[$s]}"
  done
  echo "boards:   ${!FQBN[*]}"
  exit 0
fi
if [[ $# -gt 0 ]]; then
  # Interpret leading non-flag args as sketch/board selectors. Anything after a
  # literal -- is appended to the compiler flags, e.g.
  #   EXTRA_DEFINES=-DNO_BUTTON ./scripts/build.sh MozziUSBSynth pico
  # which makes MozziUSBSynth loop its envelope instead of waiting for a button
  # on GP15 (useful on a board with nothing wired to it).
  args=("$@")
  i=0
  seen_sep=0
  while [[ $i -lt ${#args[@]} ]]; do
    a=${args[$i]}
    if [[ $seen_sep -eq 1 ]]; then
      EXTRA_DEFINES="${EXTRA_DEFINES:-} $a"
    elif [[ "$a" == "--" ]]; then
      seen_sep=1
    elif [[ -d "$ROOT/$a" ]]; then
      SKETCHES=("$a")
    elif [[ -n "${FQBN[$a]:-}" ]]; then
      BOARDS=("$a")
    fi
    i=$((i + 1))
  done
fi

if [[ ! -d libs/Mozzi || ! -d libs/FixMath || ! -d libs/TinyUSB_Arduino \
      || ! -d libs/Uac2Bridge ]]; then
  echo "Pinned libraries missing. Run: ./scripts/setup_libs.sh" >&2
  exit 1
fi

mkdir -p "$BUILD"
rc=0

# Isolate the compile from ~/Arduino/libraries, whose libraries dir starts empty
# so nothing can shadow the pinned --library set. AVENV_GOLDEN defaults to the
# standard data dir purely so the already-installed toolchains are reused instead
# of re-downloaded on every build; NO_ISOLATE=1 opts out.
if [[ "${NO_ISOLATE:-0}" == "1" ]]; then
  echo "note: NO_ISOLATE=1, building against the real sketchbook ($HOME/Arduino)"
else
  # shellcheck source=scripts/avenv.sh
  source "$ROOT/scripts/avenv.sh"
  export AVENV_GOLDEN="${AVENV_GOLDEN:-$HOME/.arduino15}"
  aventools_init mozziusb
  # avenv gives an empty libraries dir, so a core this build needs may not be
  # installed there yet. Pull just the ones the selected boards need; with
  # AVENV_GOLDEN set this is a fast no-op for anything already installed.
  cores=()
  for board in "${BOARDS[@]}"; do
    pf=${FQBN[$board]%%:*}
    cores+=("${pf%%:*}:${pf#*:}")
  done
  # GIGA only: Mozzi's mbed port pulls in Arduino_AdvancedAnalog, which is not a
  # dependency of anything else here. Kept explicit rather than installed into
  # every build.
  if [[ -n "${FQBN[giga]:-}" ]] && printf '%s\n' "${BOARDS[@]}" | grep -qx giga; then
    arduino-cli lib install Arduino_AdvancedAnalog@1.5.0 >/dev/null 2>&1 \
      || echo "warning: could not install Arduino_AdvancedAnalog (needed for giga)" >&2
  fi
  for c in $(printf '%s\n' "${cores[@]}" | sort -u); do
    arduino-cli core install "$c" >/dev/null 2>&1 \
      || echo "warning: could not install core $c (is its board manager URL configured?)" >&2
  done
fi

for sketch in "${SKETCHES[@]}"; do
  dir="$ROOT/$sketch"
  [[ -d "$dir" ]] || { echo "SKIP $sketch (no such directory)"; continue; }
  for board in "${BOARDS[@]}"; do
    [[ -n "${FQBN[$board]:-}" ]] || { echo "SKIP $sketch/$board (unknown board)"; continue; }
    if [[ -n "${SKETCH_BOARDS[$sketch]:-}" ]]; then
      printf '%s\n' "${SKETCH_BOARDS[$sketch]}" | grep -qx "$board" || continue
    fi
    out="$BUILD/${sketch}_${board}"
    rm -rf "$out"
    flags="-D${DEFINE[$board]} -DCFG_TUSB_DEBUG=0 ${EXTRA_DEFINES:-}"
    # The filename is tusb_config_arduinotinyusb.h, not tusb_config.h, because
    # that is the name the library's *active config slot* uses
    # (src/tusb_config_arduinotinyusb.h). One name, two consumers: this -D for
    # arduino-cli, and a plain file copy for IDE users, who cannot pass -D at
    # all because the IDE has no UI for extra build flags. See
    # scripts/ide_setup.sh, and the Arduino IDE section of the README.
    if [[ -f "$dir/tusb_config_arduinotinyusb.h" ]]; then
      flags+=" -DARDUINO_TINYUSB_CONFIG_FILE=\"$dir/tusb_config_arduinotinyusb.h\""
    fi
    echo "=================== building $sketch ($board) ==================="
    if arduino-cli compile \
        --fqbn "${FQBN[$board]}" \
        --library "$ROOT/libs/Mozzi" \
        --library "$ROOT/libs/FixMath" \
        --library "$ROOT/libs/Uac2Bridge" \
        --library "$ROOT/libs/TinyUSB_Arduino" \
        --build-path "$out" \
        --build-property "compiler.cpp.extra_flags=$flags" \
        --build-property "compiler.c.extra_flags=$flags" \
        --export-binaries \
        "$dir"; then
      :
    else
      echo "FAILED: $sketch ($board)" >&2
      rc=1
      continue
    fi

    # Assert on the library that actually got compiled. A build can succeed while
    # linking the wrong TinyUSB: arduino-cli resolves #include "ArduinoTinyUSB.h"
    # by searching, and only one of the candidates wins silently. The old
    # Arduino-TinyUSB is the dangerous one -- it compiles bsp_r4.cpp, so its
    # presence here means the sketchbook shadowed libs/ and this binary is not
    # what the next person will get.
    if [[ -f "$out/libraries/Arduino-TinyUSB/ArduinoTinyUSB/bsp_r4.cpp.o" ]]; then
      echo "FAILED: $sketch ($board) linked Arduino-TinyUSB, not the pinned TinyUSB_Arduino" >&2
      rc=1
      continue
    fi
    if ! ls -d "$out"/libraries/TinyUSB_Arduino >/dev/null 2>&1; then
      echo "FAILED: $sketch ($board) did not compile the pinned TinyUSB_Arduino" >&2
      rc=1
      continue
    fi
    echo "  -> $out"
  done
done
exit $rc
