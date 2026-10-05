#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 controllercustom@myyahoo.com
# Part of Mozzi-tinyusb. See LICENSE at the repository root.

# hw_test.sh — flash a sketch and verify the audio the host actually receives.
#
#   scripts/hw_test.sh <sketch> [board]        board defaults to pico
#
# Recording is the assertion: a UAC2 capture device can enumerate perfectly and
# still deliver the wrong audio (that is exactly how this project's first
# hardware test went), so every check is made on a real arecord capture.
#
# Wiring: the Debug Probe's UART must be on GP0/GP1/GND for the console, and
# SWD for flashing. Probe serial is auto-detected from lsusb.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

SKETCH=${1:-SineToneTest}
BOARD=${2:-pico}
OUT=${TMPDIR:-/tmp}/mozziusb-hw
mkdir -p "$OUT"

# ---- board family: probe, openocd tree, target, console source ------------
# The three boards do not share a probe, an OpenOCD build, or even where the
# console comes from, so this is a table rather than a single hardcoded path.
case $BOARD in
  pico|pico2)
    OCD=$HOME/.arduino15/packages/rp2040/tools/pqt-openocd/5.0.0-9576866/bin/openocd
    OCD_SCRIPTS=""                # this build finds its own scripts
    IFACE="interface/cmsis-dap.cfg"
    TRANSPORT="transport select swd"
    ADAPTER_SPEED=4000
    PROBE_IDS="2e8a:000c"          # Debug Probe
    # Console is the probe's own UART on GP0/GP1.
    CONSOLE_ON="probe"
    case $BOARD in
      pico)  TARGETS="target/rp2040.cfg" ;;
      pico2) TARGETS="target/rp2350.cfg" ;;
    esac
    ;;
  giga)
    # GIGA is flashed by an STLINK-V3 over SWD. Two things differ from the Pico
    # path and both fail confusingly if assumed:
    #  * interface/stlink.cfg is the *hla* driver in the system OpenOCD tree and
    #    dies with "Unsupported transport". stlink-dap.cfg is the native
    #    st-link driver and wants dapdirect_swd, not swd.
    #  * the console is NOT the probe. The STLINK-V3 presents its own VCP as a
    #    ttyACM node; the GIGA's Serial1 is on a CP210x bridge as ttyUSB. Reading
    #    the probe's VCP looks like it works and yields nothing.
    OCD=/usr/bin/openocd
    OCD_SCRIPTS=/usr/share/openocd/scripts
    IFACE="interface/stlink-dap.cfg"
    TRANSPORT="transport select dapdirect_swd"
    ADAPTER_SPEED=1000
    PROBE_IDS="0483:374e 0483:374f 0483:3753"   # STLINK-V3 in normal modes
    CONSOLE_ON="cp210x"
    TARGETS="target/stm32h7x.cfg"
    ;;
  *)
    echo "board $BOARD: unknown" >&2; exit 1 ;;
esac
[[ -x $OCD ]] || { echo "openocd not found at $OCD" >&2; exit 1; }
OCD_S=()
[[ -n $OCD_SCRIPTS ]] && OCD_S=(-s "$OCD_SCRIPTS")

# ---- which USB device to look for ------------------------------------------
# Not one global PID. MozziGigaSynth deliberately presents 2341:5002 so a GIGA
# stays distinguishable from a Pico on the same host, so the VID:PID to match is a
# property of the sketch, not the board. Sketches absent from the table use the
# repo default 2341:5001.
declare -A SKETCH_USB_ID=(
  [MozziGigaSynth]="2341:5002"
)
USB_ID=${SKETCH_USB_ID[$SKETCH]:-2341:5001}

# ---- which probe is on this board -----------------------------------------
# Probe<->board wiring is not discoverable in software, and two boards with the
# same firmware are attached at once, so probe serials are matched by asking the
# probe whether it can see the chip we expect: the target script only initialises
# against the right silicon. Set PROBE_SERIAL to skip the search.
probe_serials() {
  local d id
  for d in /sys/bus/usb/devices/*/; do
    id="$(cat "$d/idVendor" 2>/dev/null):$(cat "$d/idProduct" 2>/dev/null)"
    for want in $PROBE_IDS; do
      [[ $id == "$want" ]] || continue
      cat "$d/serial" 2>/dev/null
    done
  done
}

if [[ -n ${PROBE_SERIAL:-} ]]; then
  SERIAL=$PROBE_SERIAL
  TARGET=${TARGETS[0]}
else
  SERIAL=""
  for t in "${TARGETS[@]}"; do
    for s in $(probe_serials); do
      if "$OCD" "${OCD_S[@]}" -f "$IFACE" -c "$TRANSPORT" \
                 -c "adapter speed $ADAPTER_SPEED" -c "adapter serial $s" \
                 -f "$t" -c "init; shutdown" 2>&1 \
           | grep -q "processor detected"; then
        SERIAL=$s
        TARGET=$t
        break 2
      fi
    done
  done
  [[ -n $SERIAL ]] || { echo "no probe could see a $TARGETS chip; set PROBE_SERIAL=..." >&2; exit 1; }
fi
echo "probe: $SERIAL (target $TARGET)"

ELF="$ROOT/build/${SKETCH}_${BOARD}/${SKETCH}.ino.elf"
[[ -f $ELF ]] || { echo "no build at $ELF - run: ./scripts/build.sh $SKETCH $BOARD" >&2; exit 1; }

# Resolve the console by USB serial, never by tty name. Two probes are attached
# during development and "first ttyACM" is a race that silently captures the other
# board's console -- whose telemetry looks entirely plausible and describes a
# different chip. The same trap on the GIGA: the STLINK-V3's own VCP is a ttyACM
# node, and it is not the board.
tty_for_serial() {
  # $1 = USB serial to match.
  local want=$1 d p dev
  for d in /sys/class/tty/ttyACM* /sys/class/tty/ttyUSB*; do
    [[ -e $d ]] || continue
    # Walk up from the tty's device link until a USB device node with a serial
    # turns up (the tty sits on an interface node, whose parent is the device).
    p=$(readlink -f "$d/device")
    while [[ -n $p && $p != / ]]; do
      dev="/sys/bus/usb/devices/$(basename "$p")"
      if [[ -r $dev/idVendor && -r $dev/serial ]]; then
        if [[ $(cat "$dev/serial") == "$want" ]]; then
          echo "/dev/${d##*/}"
          return 0
        fi
        break
      fi
      p=$(dirname "$p")
    done
  done
  return 1
}

probe_tty() { tty_for_serial "$1"; }

# The GIGA's Serial1 console sits behind a CP210x on the board, so match on the
# bridge's VID:PID rather than a serial string (which is the generic
# "CP2102_USB_to_UART_Bridge_Controller_0001" and can differ per adapter).
tty_for_vidpid() {
  local want="$1" d p dev
  for d in /sys/class/tty/ttyACM* /sys/class/tty/ttyUSB*; do
    [[ -e $d ]] || continue
    p=$(readlink -f "$d/device")
    while [[ -n $p && $p != / ]]; do
      dev="/sys/bus/usb/devices/$(basename "$p")"
      if [[ -r $dev/idVendor && -r $dev/idProduct ]]; then
        if [[ "$(cat "$dev/idVendor"):$(cat "$dev/idProduct")" == "$want" ]]; then
          echo "/dev/${d##*/}"
          return 0
        fi
        break
      fi
      p=$(dirname "$p")
    done
  done
  return 1
}

# Arm the console BEFORE flashing: the flash's "reset run" is what makes the
# board print, and the banner lands within a second of it.
case $CONSOLE_ON in
  probe)  TTY=$(probe_tty "$SERIAL" || true) ;;
  cp210x) TTY=$(tty_for_vidpid "10c4:ea60" || true) ;;
esac
if [[ -n $TTY ]]; then
  echo "console: $TTY"
  stty -F "$TTY" raw 115200 -hupcl 2>/dev/null || true
  nohup timeout 60 cat "$TTY" > "$OUT/$SKETCH.console.log" 2>/dev/null &
  CAT_PID=$!
  sleep 1
else
  echo "console: probe $SERIAL tty not found, continuing without it" >&2
fi

echo "== flashing $SKETCH ($BOARD) =="
# reset run, not just program: program alone leaves the target halted, the USB
# device stays stale, and arecord then fails with "unable to install hw params"
# against firmware that is actually fine.
#
# The explicit init matters. Letting `program` do the init itself fails on the
# RP2350 with "embedded:startup.tcl:72: Error: Can't find rp2350" when another
# openocd instance (the probe search above) attached just beforehand and left
# the target halted.
"$OCD" "${OCD_S[@]}" -f "$IFACE" -c "$TRANSPORT" -c "adapter speed $ADAPTER_SPEED" \
       -c "adapter serial $SERIAL" -f "$TARGET" \
       -c "init" -c "program $ELF verify; reset run; shutdown" 2>&1 \
  | grep -E "Verified|Error" || true

sleep 3

# Pick the capture card by USB port, never by enumeration order: with two boards
# running the same firmware, card 1 vs card 2 is not stable and picking the
# wrong one silently records the other board. Override with CARD=N when more
# than one device matches.
board_cards() {
  local c p dev
  for c in /sys/class/sound/card[0-9]*; do
    [[ -e $c ]] || continue
    p=$(readlink -f "$c/device")
    while [[ -n $p && $p != / ]]; do
      dev="/sys/bus/usb/devices/$(basename "$p")"
      if [[ -r $dev/idVendor && -r $dev/idProduct ]]; then
        if [[ "$(cat "$dev/idVendor"):$(cat "$dev/idProduct")" == "$USB_ID" ]]; then
          echo "${c##*/card} $(basename "$p")"
        fi
        break
      fi
      p=$(dirname "$p")
    done
  done
}

mapfile -t CANDIDATES < <(board_cards)
if [[ -n ${CARD:-} ]]; then
  CARD_N=$CARD
elif [[ ${#CANDIDATES[@]} -eq 1 ]]; then
  CARD_N=${CANDIDATES[0]%% *}
elif [[ ${#CANDIDATES[@]} -eq 0 ]]; then
  echo "no capture card found for $USB_ID" >&2
  exit 1
else
  echo "more than one Mozzi device attached; pass CARD=<n>:" >&2
  for c in "${CANDIDATES[@]}"; do echo "  card${c%% *} at USB ${c##* }" >&2; done
  exit 1
fi

# Wait for the device to be enumerable AND its PCM actually openable for capture,
# re-resolving the card each round. Both halves were measured the hard way on the
# GIGA: a flash resets the board and the kernel then takes several seconds to cycle
# it -- "usb disconnect" -> "new full-speed USB device" -> "New USB device found"
# was 3.6 s + 5.1 s = ~8.7 s in dmesg -- and during that the card index can shift.
# The card also appears in /proc/asound before snd-usb-audio has created its PCM,
# after which arecord fails with "audio open error: No such file or directory"
# against firmware that is fine.
#
# The probe is a real capture, not a readiness query. `arecord --dump-hw-params`
# is not usable for this: it prints the params and then exits 1 regardless, so
# testing its exit status fails on a perfectly good device. A failed capture open
# costs ~0.1 s, so retrying the actual capture is both correct and cheap.
RAW="$OUT/$SKETCH.raw"
ready=0
for attempt in $(seq 1 40); do
  mapfile -t CANDIDATES < <(board_cards)
  for c in ${CANDIDATES+"${CANDIDATES[@]}"}; do
    [[ "$c" == "$CARD_N "* ]] && break
    # Not the index we were told about. If exactly one device matches, prefer the
    # one we can positively identify over a stale index and say so -- silently
    # recording the wrong board is what this whole selection logic prevents.
    if [[ ${#CANDIDATES[@]} -eq 1 ]]; then
      CARD_N=${CANDIDATES[0]%% *}
      echo "note: card index moved to $CARD_N for $USB_ID; using it"
      break
    fi
  done
  if arecord -D "hw:$CARD_N,0" -f S16_LE -r 48000 -c 1 -d 6 "$RAW" 2>/dev/null; then
    ready=1
    break
  fi
  rm -f "$RAW"
  sleep 0.5
done
if [[ $ready -ne 1 ]]; then
  echo "no openable capture card for $USB_ID after ~20 s; firmware may not be enumerating" >&2
  exit 1
fi
echo "== recorded $RAW =="

echo "== console =="
cat "$OUT/$SKETCH.console.log" 2>/dev/null | head -20 || true

echo "== analysing =="
case $SKETCH in
  SineToneTest)
    # 127 * 72 = 9144 peak, /sqrt(2) = 6466 RMS = -14.10 dBFS
    python3 tools/analyze_capture.py "$RAW" --expect-hz 440 --expect-dbfs -14.1
    ;;
  MozziUSBSynth)
    # Two oscillators 7 cents apart beat against each other by design.
    python3 tools/analyze_capture.py "$RAW" --expect-hz 110 --detune-cents 7 \
            --min-rms -20
    ;;
  PatternTest)
    # Whole-file analysis is meaningless here: PatternTest deliberately mixes DC
    # constant blocks with the sine, so the RMS is a blend and the "spur" is just
    # the DC blocks' energy. Assert per-block instead -- that is the transport
    # property this sketch exists to test. The block phase is arbitrary relative
    # to the start of the capture, so the checker anchors on the 0x5A5A block
    # rather than assuming block 0 lines up with sample 0.
    python3 tools/check_pattern_blocks.py "$RAW" "$SKETCH/$BOARD"
    ;;
  MozziGigaSynth)
    # Two assertions deliberately withheld. Pitch comes from the nanoKONTROL2 CC
    # cache, which starts at the built-in defaults and is whatever the controller
    # last sent once one is attached, so no frequency is defined. And the spur
    # floor is not a property of this sketch: a filtered saw+square pair with two
    # detuned oscillators has strong harmonics by construction (measured -8.2 dBc),
    # so bounding it would assert nothing. What IS asserted is that the GIGA's
    # simultaneous device+host stack is actually carrying audio: level in range,
    # no clipping, and the energy in band rather than aliased. A fixed-pitch check
    # belongs on SineToneTest.
    python3 tools/analyze_capture.py "$RAW" --min-rms -24 --ignore-spur
    ;;
  *)
    python3 tools/analyze_capture.py "$RAW"
    ;;
esac

# Stop the console reader before exiting. Left running it keeps the tty claimed
# and interleaves with the next run's reader, which splices the log into
# unreadable garbage.
if [[ -n ${CAT_PID:-} ]]; then
  kill "$CAT_PID" 2>/dev/null || true
  wait "$CAT_PID" 2>/dev/null || true
fi
