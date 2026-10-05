#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 controllercustom@myyahoo.com
# Part of Mozzi-tinyusb. See LICENSE at the repository root.

# Prepare the Arduino sketchbook so the IDE can build a sketch of this project,
# then install that sketch's TinyUSB config into the library.
#
#   ./scripts/ide_setup.sh                  # report what is present / missing
#   ./scripts/ide_setup.sh SineToneTest     # install that sketch's config
#   ./scripts/ide_setup.sh --restore        # put the library's own config back
#   ./scripts/ide_setup.sh --check SineToneTest   # non-mutating; exit 1 if not ready
#
# Why this exists: the Arduino IDE has no UI for extra compiler flags, so the
# documented -DARDUINO_TINYUSB_CONFIG_FILE route is unavailable to exactly the
# people this script is for. TinyUSB_Arduino's own answer is a file copy -- see
# its README, "Optimizing Flash and RAM" -- and that is what this automates.
#
# Two traps it checks that scripts/build.sh cannot, because the IDE can neither
# pass --library explicitly nor assert afterwards which library was compiled: an
# older Arduino-TinyUSB shadowing TinyUSB_Arduino, and upstream Mozzi installed
# where the fork is required (both report version 2.0.4, so the version cannot
# tell them apart).
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD

SKETCHBOOK="${ARDUINO_DIRECTORIES_USER:-$HOME/Arduino}"
LIBS="$SKETCHBOOK/libraries"

# Libraries this project needs, and where each one comes from. Only FixMath and
# Arduino_AdvancedAnalog are in the Arduino Library Manager; the other two are
# not published there, so they are installed from a zip.
declare -A NEEDED=(
  [Uac2Bridge]="first-party, now its own repo: Download ZIP from https://github.com/controllercustom/Uac2Bridge (MIT, same holder as this repo)"
  [TinyUSB_Arduino]="zip from https://github.com/controllercustom/TinyUSB_Arduino"
  [Mozzi]="zip from https://github.com/controllercustom/Mozzi (the fork; GIGA needs it)"
  [FixMath]="Library Manager: arduino-cli lib install FixMath@1.0.9"
)
# Only GIGA needs this one, via Mozzi's mbed port.
GIGA_ONLY=Arduino_AdvancedAnalog

# Print this file's header comment as usage, minus the SPDX/copyright
# boilerplate. Derived from the file rather than a hardcoded line range, so it
# cannot drift out of sync when the header changes.
usage() {
  awk '
    /^#!/                          { next }
    /^# (SPDX|Copyright|Part of)/    { next }
    !/^#/ && seen                   { exit }
    /^#/                            { sub(/^# ?/, ""); print; seen = 1 }
  ' "$0"
}

# The config each sketch wants installed, and the library slot it goes into.
config_src_for() { printf '%s/%s/tusb_config_arduinotinyusb.h\n' "$ROOT" "$1"; }
active_config() { printf '%s/%s/src/tusb_config_arduinotinyusb.h\n' "$LIBS" TinyUSB_Arduino; }
union_config()  { printf '%s/%s/src/config/tusb_config_union.h\n' "$LIBS" TinyUSB_Arduino; }
backup_config() { printf '%s/src/.tusb_config_arduinotinyusb.h.orig\n' "$LIBS/TinyUSB_Arduino"; }

# ---------------------------------------------------------------------------

report() {
  echo "sketchbook: $SKETCHBOOK"
  echo
  local missing=0 lib
  for lib in "${!NEEDED[@]}"; do
    if [[ -d "$LIBS/$lib" ]]; then
      printf '  [ok]      %-18s %s\n' "$lib" "$(ver_of "$LIBS/$lib")"
    else
      printf '  [MISSING] %-18s %s\n' "$lib" "${NEEDED[$lib]}"
      missing=$((missing + 1))
    fi
  done
  if [[ -d "$LIBS/$GIGA_ONLY" ]]; then
    printf '  [ok]      %-18s %s\n' "$GIGA_ONLY" "$(ver_of "$LIBS/$GIGA_ONLY")"
  else
    printf '  [note]    %-18s %s\n' "$GIGA_ONLY" "only needed for GIGA; Library Manager"
  fi

  # Version numbers cannot tell the Mozzi fork from upstream 2.0.4 -- both say
  # 2.0.4 -- so test the thing that actually differs: the fork carries an
  # EXTERNAL_CUSTOM branch in the mbed guts, and without it a GIGA build fails
  # with "'startAudio' was not declared in this scope".
  if [[ -d "$LIBS/Mozzi" ]]; then
    local guts="$LIBS/Mozzi/internal/MozziGuts_impl_MBED.hpp"
    if [[ -f "$guts" ]] && grep -q MOZZI_OUTPUT_EXTERNAL_CUSTOM "$guts"; then
      printf '  [ok]      %-18s %s\n' "Mozzi fork" "mbed EXTERNAL_CUSTOM present"
    else
      cat >&2 <<WARN

  [WARNING] the Mozzi in $LIBS looks like upstream, not the controllercustom fork.
            Both report version 2.0.4, so the version cannot tell them apart.
            Upstream has no MOZZI_OUTPUT_EXTERNAL_CUSTOM branch in its mbed guts,
            which is the audio mode this project is built on, so a GIGA build
            fails with:  'startAudio' was not declared in this scope
            Fix: remove it and install https://github.com/controllercustom/Mozzi

WARN
    fi
  fi

  # The silent one. Both libraries ship ArduinoTinyUSB.h; whichever the
  # directory scan reaches first wins. A < sorts before T, so the old one wins.
  if [[ -d "$LIBS/Arduino-TinyUSB" ]]; then
    cat >&2 <<WARN

  [WARNING] Arduino-TinyUSB is also installed in $LIBS.
            It and TinyUSB_Arduino both ship ArduinoTinyUSB.h, and arduino-cli
            picks whichever the directory scan reaches first, with no warning.
            "Arduino-TinyUSB" sorts before "TinyUSB_Arduino", so the old stack
            is the one that gets compiled. Verified: with both present, the
            build compiled Arduino-TinyUSB and not TinyUSB_Arduino.
            Remove it, or the firmware you get is not the firmware you read.

WARN
  fi
  return $((missing > 0))
}

ver_of() {
  local v
  v=$(sed -n 's/^version=//p' "$1/library.properties" 2>/dev/null | head -1)
  printf 'v%s' "${v:-?}"
}

# Install one sketch's config over the library's active one. Idempotent: if the
# right bytes are already there, say so and do nothing.
install_config() {
  local sketch=$1 src dst
  src=$(config_src_for "$sketch")
  dst=$(active_config)

  if [[ ! -d "$ROOT/$sketch" ]]; then
    echo "no such sketch: $sketch" >&2
    return 1
  fi
  if [[ ! -f "$src" ]]; then
    echo "missing $src" >&2
    echo "This project names its per-sketch config tusb_config_arduinotinyusb.h," >&2
    echo "because that is the filename the library's active config slot expects." >&2
    return 1
  fi
  if [[ ! -f "$dst" ]]; then
    echo "missing $dst -- is TinyUSB_Arduino installed?" >&2
    return 1
  fi
  if cmp -s "$src" "$dst"; then
    echo "config already installed: $sketch"
    return 0
  fi
  # Back up the library's own active config before overwriting it. Not optional:
  # the library's documented way back is "cp config/tusb_config_union.h
  # src/tusb_config_arduinotinyusb.h", and that is NOT a faithful restore. At
  # 186129d the union is 224 lines and the active config is 205 -- they are
  # duplicated content that has drifted, so the union copy silently rewrites the
  # library's shipped config. Restoring the exact bytes is the only safe undo.
  local bak
  bak=$(backup_config)
  if [[ ! -f "$bak" ]]; then
    cp "$dst" "$bak"
    echo "backed up the library's own config -> $bak"
  fi
  cp "$src" "$dst"
  echo "installed $sketch's config -> $dst"
  local tuh
  tuh=$(sed -n 's/^#define CFG_TUH_ENABLED //p' "$src" | head -1)
  cat <<NOTE
  The config is per sketch, and this one's CFG_TUH_ENABLED is ${tuh:-unset}, so
  run this again before building a different sketch:
      ./scripts/ide_setup.sh <OtherSketch>
  To put the library's own config back:
      ./scripts/ide_setup.sh --restore
NOTE
}

check_config() {
  local sketch=$1 src dst
  src=$(config_src_for "$sketch"); dst=$(active_config)
  [[ -f "$src" && -f "$dst" ]] && cmp -s "$src" "$dst"
}

restore_union() {
  local u d b
  u=$(union_config); d=$(active_config); b=$(backup_config)
  if [[ -f "$b" ]]; then
    cp "$b" "$d"; rm -f "$b"
    echo "restored the library's own config -> $d"
    if [[ -f "$u" ]] && ! cmp -s "$u" "$d"; then
      cat >&2 <<NOTE
  Note: that is NOT the same as copying the library's union config over it
  (config/tusb_config_union.h, 224 lines). The union and the library's own
  active config have drifted -- $(wc -l <"$d") vs $(wc -l <"$u") lines -- so the
  union copy is what its README suggests but it rewrites the shipped config.
  The bytes just restored are the ones that were there before this script ran.
NOTE
    fi
    return 0
  fi
  # No backup: either nothing was ever installed, or the library was reinstalled
  # since. Fall back to the library's documented step, but say what it costs.
  [[ -f "$u" ]] || { echo "missing $u and no backup to restore from" >&2; return 1; }
  cp "$u" "$d"
  echo "no backup found -- installed the library's union config instead"
  echo "That is the library's documented undo, but it is lossy: the union and"
  echo "the shipped active config have drifted, so this is not byte-identical to"
  echo "a fresh install of TinyUSB_Arduino."
}

# ---------------------------------------------------------------------------

ACTION=report
SKETCH=""
case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  --restore) ACTION=restore; shift ;;
  --check)   ACTION=check; SKETCH="${2:-}"; shift $(( $#>1 ? 2 : 1 )) ;;
  -*)        echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  "")        ACTION=report ;;
  *)         ACTION=install; SKETCH="$1"; shift ;;
esac

case "$ACTION" in
  report) report || true ;;
  install) report >/dev/null 2>&1 || true; install_config "$SKETCH" ;;
  check)
    report >/dev/null 2>&1 || true
    rc=0
    for lib in "${!NEEDED[@]}"; do
      [[ -d "$LIBS/$lib" ]] || { echo "not ready: $lib missing from $LIBS" >&2; rc=1; }
    done
    if [[ -n "$SKETCH" ]]; then
      if check_config "$SKETCH"; then
        echo "config installed: $SKETCH"
      else
        echo "not ready: $SKETCH's config is not the active one in the library" >&2
        rc=1
      fi
    fi
    exit $rc
    ;;
  restore) restore_union ;;
esac