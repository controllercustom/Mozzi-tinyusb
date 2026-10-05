#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 controllercustom@myyahoo.com
# Part of Mozzi-tinyusb. See LICENSE at the repository root.

# Fetch the pinned third-party libraries this project builds against.
#
# They live in ./libs rather than the Arduino sketchbook so a build here does not
# depend on (or disturb) ~/Arduino/libraries, and so the versions are recorded in
# the project. For Arduino IDE 2.x use, install them into the sketchbook instead:
#   arduino-cli lib install Mozzi@2.0.4 FixMath@1.0.9
#
# TinyUSB_Arduino is the load-bearing one. scripts/build.sh passes it with an
# explicit --library because arduino-cli picks a library by scanning for whoever
# provides the included header, and when two libraries both ship
# ArduinoTinyUSB.h it silently takes the first one in directory order with no
# warning. Without the explicit --library this build would quietly use whatever
# the sketchbook happens to contain.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD

# Mozzi is pinned to a FORK, not upstream. config_checks_mbed.h advertises
# MOZZI_OUTPUT_EXTERNAL_CUSTOM but MozziGuts_impl_MBED.hpp never implements it, so
# no mbed (GIGA) sketch can use the external-custom audio mode this project is
# built on: startAudio()/stopMozzi() are undefined and the build fails. The fork
# carries that one-file fix on top of upstream 7f1e5b4. Once upstream takes it,
# point MOZZI_REPO back at sensorium/Mozzi and bump to the accepting revision.
MOZZI_REPO=https://github.com/controllercustom/Mozzi
MOZZI_SHA=e392692a0401add2572d00b39c036df05c815641  # 7f1e5b4 + mbed EXTERNAL_CUSTOM
FIXMATH_REPO=https://github.com/tomcombriat/FixMath
FIXMATH_SHA=21f3f8d48e91eb77fbc4097b352ab76a3a9964ec  # v1.0.9

# Uac2Bridge is FIRST-PARTY, not a third-party dependency: it was extracted from
# this repo into its own repository so that anyone writing USB audio can use the
# rate converter without adopting the sketches. It lives in libs/ here like the
# rest, pinned by SHA like the rest, so a build is reproducible and the pinned
# revision is recorded in this file rather than in someone's working tree.
#
# Override UAC2BRIDGE_REPO to build against a local checkout instead. That is how
# this repo builds until the library is pushed:
#   UAC2BRIDGE_REPO=~/Uac2Bridge ./scripts/setup_libs.sh
UAC2BRIDGE_REPO=${UAC2BRIDGE_REPO:-https://github.com/controllercustom/Uac2Bridge}
UAC2BRIDGE_SHA=${UAC2BRIDGE_SHA:-a76bcb9ff4e06c695cd5c739ed7c494304d2b073}  # initial commit
TINYUSB_REPO=https://github.com/controllercustom/TinyUSB_Arduino
# Untagged, library.properties version=1.0.0. Pin by SHA because there is no tag
# to pin to: the repo calls itself experimental and warns it may not track
# upstream, so a moving ref would make builds unreproducible. Bumping this SHA is
# a deliberate act, not something to let happen by accident.
#   c246c15  v1.0.0, initial commit
#   f8927bc  library-wide CFG_TUSB_DEBUG default 2 -> 0 (upstream TinyUSB's own
#            default). Config headers, README and CHANGELOG only -- no code paths
#            touched, so audio/host behaviour is unchanged. It removes a footgun
#            rather than adding a feature: the old default logged from inside the
#            USB ISR, where a 40-char line at 115200 baud blocks ~3.5 ms, longer
#            than a 1 ms isochronous frame. Our builds already forced 0 three
#            ways (build.sh -D, each sketch's tusb_config.h, and the explicit
#            -DARDUINO_TINYUSB_CONFIG_FILE), so this makes the IDE path safe too.
#            Verified: all 8 sketch/board flash sizes byte-identical across the bump.
#   186129d  Per-sketch specialised TinyUSB configs. Adds
#            src/config/tusb_config_union.h and replaces each example's
#            tusb_config.h with tusb_config_arduinotinyusb.h; in
#            ArduinoTinyUSB.h, moves #include "tusb.h" up above the API
#            declarations and wraps those in #if CFG_TUD_ENABLED /
#            CFG_TUH_ENABLED so a single-role config cannot link against a role
#            it does not compile in. All four sketches define both role macros
#            explicitly, so the guards resolve correctly.
#            Verified unaffected: src/tusb_option.h (so -DARDUINO_TINYUSB_CONFIG_FILE
#            still selects a config), src/tusb_config.h and
#            src/tusb_config_arduinotinyusb.h (byte-identical blob), and the whole
#            vendored TinyUSB tree -- device/, class/, host/, portable/ all
#            untouched, still b80f1c10 per the new src/VERSION.
#            Caveat at this pin: src/VERSION and the union header both say
#            src/tusb_config_arduinotinyusb.h is "one line: it includes this
#            file". It is not -- it is still the standalone 205-line config, and
#            config/tusb_config_union.h (224 lines) is included by nothing. The
#            two have duplicated content that can drift, so the library's
#            documented undo (copy the union over the active config) is lossy
#            here. scripts/ide_setup.sh sidesteps it by restoring its own backup.
#   c6b84e2  AVAILABLE, NOT PINNED. "Fix duplicate configs" -- the fix for the
#            caveat above, and the only commit past this pin. -201/+53 across
#            exactly src/config/tusb_config_union.h and
#            src/tusb_config_arduinotinyusb.h: the active config drops 249 -> 52
#            lines and becomes the one-line #include of the union its own docs
#            always described. Verified by reading the public repo.
#            Bumping owes the usual proof -- baseline, bump, --full, all 19
#            sketch/board pairs byte-identical -- so it is a deliberate act.
TINYUSB_SHA=186129d2ac5a1fe2cf59cad5c868d2eef1de53c4  # c246c15 + CFG_TUSB_DEBUG 0 + specialised configs

fetch() {
  local url="$1" sha="$2" dir="$3"
  # A directory that exists but is not a git clone: libs/ is gitignored, so this
  # is a tree that was vendored in by other means. Do NOT rm -rf it — it may be a
  # hand-curated Mozzi. Say what we can and cannot verify and move on.
  if [[ -d "$dir" && ! -d "$dir/.git" ]]; then
    local ver
    ver=$(sed -n 's/^version=//p' "$dir/library.properties" 2>/dev/null | head -1)
    echo "  $dir present but is not a git clone; leaving it alone (version=${ver:-unknown}, cannot verify against ${sha:0:8})"
    return
  fi
  if [[ -d "$dir/.git" ]]; then
    if [[ "$(git -C "$dir" rev-parse HEAD)" == "$sha" ]]; then
      echo "  $dir already at ${sha:0:8}"
      return
    fi
    echo "  $dir at wrong revision, refetching"
    rm -rf "$dir"
  fi
  git clone --quiet "$url" "$dir"
  git -C "$dir" checkout --quiet "$sha"
  echo "  $dir -> $(git -C "$dir" rev-parse --short HEAD)"
}

mkdir -p libs
echo "Pinned libraries:"
fetch "$MOZZI_REPO"       "$MOZZI_SHA"       libs/Mozzi
fetch "$FIXMATH_REPO"     "$FIXMATH_SHA"     libs/FixMath
fetch "$TINYUSB_REPO"     "$TINYUSB_SHA"     libs/TinyUSB_Arduino
fetch "$UAC2BRIDGE_REPO"  "$UAC2BRIDGE_SHA"  libs/Uac2Bridge

# Mozzi's library.properties declares depends=FixMath, but its examples/ and
# extras/ are not needed to build and only slow down IDE indexing.
rm -rf libs/Mozzi/examples libs/Mozzi/extras
# ~34 example sketches, none of which this project uses. Note that since 186129d
# the library's per-example config is
# libraries/TinyUSB_Arduino/examples/<Name>/tusb_config_arduinotinyusb.h (it was
# <Name>/tusb_config.h before that), and it is inert where it sits -- you are
# meant to copy it over src/tusb_config_arduinotinyusb.h to activate it. This
# project does neither: it keeps its own config next to each sketch and points
# ARDUINO_TINYUSB_CONFIG_FILE at it, which is selected directly by
# src/tusb_option.h and so does not involve the copy-over dance at all.
rm -rf libs/TinyUSB_Arduino/examples

echo
echo "Versions:"
grep -H '^version=' libs/Mozzi/library.properties libs/FixMath/library.properties \
      libs/TinyUSB_Arduino/library.properties libs/Uac2Bridge/library.properties \
  | sed 's|libs/||; s|/library.properties:version=| |'
