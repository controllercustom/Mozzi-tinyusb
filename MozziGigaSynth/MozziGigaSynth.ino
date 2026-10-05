// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com
// Part of Mozzi-tinyusb. See LICENSE at the repository root.

// MozziGigaSynth.ino — monophonic lead/bass synth for the GIGA R1, with
// nanoKONTROL2 control in and 48 kHz UAC2 capture out.
//
// Chain: Mozzi EXTERNAL_CUSTOM (32768 Hz, loop-paced) -> Uac2Bridge ->
// UAC2 EP IN (Type-C device, rhport 0) + nanoK MIDI host (Type-A, rhport 1).
//
// The GIGA is the one board where TinyUSB runs device and host at once, on two
// independent controllers (OTG_FS / OTG_HS), which is why both tud_ and tuh_ are
// pumped below. On every other board TinyUSB owns the single native port and is
// either a device or a host, never both — so this sketch is giga-only and
// scripts/build.sh skips it elsewhere.
//
// Faders 0-7: cutoff, resonance, attack, decay, sustain, release, LFO-rate, volume
// Knobs 16-23: pitch, detune, osc-mix, filter-EG, LFO-depth, drive, (spare), vibrato
// Solo 0-2: octave -1/0/+1 | Mute 0: LFO on/off | Play/Stop: gate | Rec 0: retrigger
//
// GIGA rules (load-bearing — the second one was found by a HardFault):
// - Serial1.begin() BEFORE any tud/tuh init. tud_arduino_init() claims OTG_FS,
//   and a Mbed UART touched after that faults in mbed::SerialBase::writeable().
//   setup() below does the console first for exactly this reason.
// - Console is Serial1 (CP210x on D0/D1), not the probe: the STLINK-V3 presents
//   its own VCP as a ttyACM node, and the board is on ttyUSB. Mbed CDC dies once
//   the device stack owns OTG_FS, so flash over SWD with the STLINK-V3.
//
// This sketch presents VID:PID 2341:5002, deliberately distinct from the 5001
// the other sketches use, so a GIGA and a Pico can be told apart on one host.
// scripts/hw_test.sh matches per board rather than assuming one PID.

#include <MozziConfigValues.h>

#define MOZZI_AUDIO_MODE       MOZZI_OUTPUT_EXTERNAL_CUSTOM
#define MOZZI_AUDIO_RATE       32768
#define MOZZI_AUDIO_BITS       16
#define MOZZI_CONTROL_RATE     256
#define MOZZI_AUDIO_INPUT      MOZZI_AUDIO_INPUT_NONE
#define MOZZI_COMPATIBILITY_LEVEL MOZZI_COMPATIBILITY_2_0
#define MOZZI_ANALOG_READ     MOZZI_ANALOG_READ_NONE

#include <Mozzi.h>
#include <Oscil.h>
#include <ADSR.h>
#include <ResonantFilter.h>
#include <Smooth.h>
#include <mozzi_midi.h>
#include <tables/saw2048_int8.h>
#include <tables/square_no_alias_2048_int8.h>
#include <tables/sin2048_int8.h>

#include <ArduinoTinyUSB.h>
#include <Uac2Bridge.h>

#include "midi_map.h"
#include "synth_params.h"

SynthParams params;

namespace {
Uac2Bridge::Bridge bridge;
// Probe for console diagnostics (written in updateAudio, read in report).
volatile int16_t lastOut = 0;// One 1 ms USB frame: 98 B = 49 samples. Must match tusb_config.h.
constexpr uint16_t kChunkSamples = Uac2Bridge::kUsbEpSamples;   // 49
int16_t chunk[kChunkSamples];

// Voice: saw lead + square sub, sine LFO (control rate).
// Table MUST go to the constructor (no-arg leaves the pointer uninitialised).
Oscil<SAW2048_NUM_CELLS, MOZZI_AUDIO_RATE> aSaw(SAW2048_DATA);
Oscil<SQUARE_NO_ALIAS_2048_NUM_CELLS, MOZZI_AUDIO_RATE> aSquare(SQUARE_NO_ALIAS_2048_DATA);
Oscil<SIN2048_NUM_CELLS, MOZZI_CONTROL_RATE> kLfo(SIN2048_DATA);
LowPassFilter lpf;
ADSR<MOZZI_CONTROL_RATE, MOZZI_CONTROL_RATE> envelope;

Smooth<uint16_t> smoothCutoff(0.85f);
Smooth<uint16_t> smoothPitch(0.9f);
uint8_t envGain = 0;

// Minor pentatonic degrees for knob pitch (no keys on the nanoKONTROL2).
const int8_t PENTA[5] = {0, 3, 5, 7, 10};
const uint8_t BASE_MIDI = 33;  // A1

uint8_t midiFromKnob(uint8_t v, int8_t octave) {
  uint8_t step = v >> 3;  // 0..15 over 3+ octaves of pentatonic
  uint8_t deg = step % 5;
  uint8_t oct = step / 5;
  int16_t m = (int16_t)BASE_MIDI + oct * 12 + PENTA[deg] + (int16_t)octave * 12;
  if (m < 21) m = 21;
  if (m > 108) m = 108;
  return (uint8_t)m;
}
}  // namespace

// ---- Mozzi callbacks (global scope: audioHook() calls them unqualified) ----
void audioOutput(const AudioOutput f) { bridge.push((int16_t)f.l()); }
inline bool canBufferAudioOutput() { return bridge.canPush(); }

void updateControl() {
  // Snapshot volatile CC cache.
  uint8_t f[8], k[8];
  for (uint8_t i = 0; i < 8; i++) {
    f[i] = ccCache[CC_FADER[i]];
    k[i] = ccCache[CC_KNOB[i]];
  }

  // Faders: cutoff, resonance, A, D, S, R, LFO-rate, volume.
  params.cutoff = f[0];
  params.resonance = f[1] >> 1;
  params.attackMs = map(f[2], 0, 127, 5, 800);
  params.decayMs = map(f[3], 0, 127, 20, 1000);
  params.sustainMs = map(f[4], 0, 127, 50, 2000);
  params.releaseMs = map(f[5], 0, 127, 20, 2000);
  params.lfoRateHz = 0.1f + (f[6] / 127.0f) * 20.0f;
  params.volume = f[7] < 4 ? 0 : map(f[7], 0, 127, 0, 255);

  // Knobs: pitch, detune, osc-mix, filter-EG, LFO-depth, drive, -, vibrato.
  uint8_t midi = midiFromKnob(k[0], params.octaveShift);
  params.oscFreq = mtof(midi);
  params.detune = (k[1] / 127.0f) * 12.0f;
  params.oscMix = k[2] << 1;
  uint8_t filterEG = k[3];
  params.lfoDepth = k[4] >> 1;
  params.drive = k[5];
  params.vibDepth = k[7] >> 2;

  // Solo 0-2: octave select. Mute 0: LFO enable toggle on press edge.
  if (ccSeen[CC_SOLO[0]] && ccCache[CC_SOLO[0]] > 0) params.octaveShift = -1;
  if (ccSeen[CC_SOLO[1]] && ccCache[CC_SOLO[1]] > 0) params.octaveShift = 0;
  if (ccSeen[CC_SOLO[2]] && ccCache[CC_SOLO[2]] > 0) params.octaveShift = 1;
  static uint8_t lastMute0 = 0;
  if (ccSeen[CC_MUTE[0]]) {
    uint8_t v = ccCache[CC_MUTE[0]];
    if (v > 0 && lastMute0 == 0) params.lfoEnable = !params.lfoEnable;
    lastMute0 = v;
  }

  float vib = params.vibDepth > 0
      ? (kLfo.next() / 128.0f) * (params.vibDepth / 32.0f)
      : 0.0f;
  float freq = smoothPitch.next(params.oscFreq + vib);
  aSaw.setFreq(freq);
  aSquare.setFreq(freq + params.detune);
  kLfo.setFreq(params.lfoRateHz);

  int16_t lfo = params.lfoEnable ? kLfo.next() : 0;  // -128..127
  int16_t modCut = (int16_t)smoothCutoff.next(params.cutoff)
      + (lfo * params.lfoDepth >> 6)
      + (filterEG >> 3);
  if (modCut < 8) modCut = 8;
  if (modCut > 255) modCut = 255;
  lpf.setCutoffFreqAndResonance((uint8_t)modCut, params.resonance);

  envelope.setTimes(params.attackMs, params.decayMs, params.sustainMs, params.releaseMs);
  envelope.update();
  envGain = envelope.next();

  // Rec 0 re-triggers; Play/Stop drive the gate.
  static uint8_t lastRec0 = 0;
  uint8_t rec0 = ccSeen[CC_REC[0]] ? ccCache[CC_REC[0]] : 0;
  if (rec0 > 0 && lastRec0 == 0) {
    envelope.noteOff();
    envelope.noteOn();
  }
  lastRec0 = rec0;
  static bool wasGate = true;
  if (params.gate && !wasGate) envelope.noteOn();
  if (!params.gate && wasGate) envelope.noteOff();
  wasGate = params.gate;
  // Drone self-heal: sustain is timed, so an un-retriggered envelope parks at
  // zero (and any stray noteOff does the same). Re-fire while gated.
  if (params.gate && !envelope.playing()) envelope.noteOn();
}

AudioOutput updateAudio() {
  int16_t saw = aSaw.next();
  int16_t sq = aSquare.next();
  int16_t mixed = ((256 - params.oscMix) * saw + params.oscMix * sq) >> 8;
  int16_t driven = mixed + ((mixed * (int16_t)params.drive) >> 7);
  int16_t filtered = lpf.next(driven);
  // 16-bit bridge scale: max 127*255*255/512 = 16129, no clipping.
  int32_t out = (int32_t)filtered * envGain * params.volume;
  out >>= 9;
  lastOut = (int16_t)out;
  return MonoOutput::from16Bit((int16_t)out);
}

// ---- TinyUSB host MIDI callbacks (nanoKONTROL2) ----
extern "C" void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *data) {
  Serial1.print(F("-- nanoK MIDI mounted idx="));
  Serial1.print(idx);
  Serial1.print(F(" addr="));
  Serial1.println(data->daddr);
}

extern "C" void tuh_midi_umount_cb(uint8_t idx) {
  Serial1.print(F("-- MIDI removed idx="));
  Serial1.println(idx);
}

extern "C" void tuh_midi_rx_cb(uint8_t idx, uint32_t /*n*/) {
  uint8_t packet[4];
  while (tuh_midi_packet_read(idx, packet)) {
    uint8_t cin = packet[0] & 0x0F;
    if (cin != 0xB) continue;  // v1: CC only
    uint8_t cc = packet[2];
    uint8_t val = packet[3];
    if (cc < 128) {
      ccCache[cc] = val;
      ccSeen[cc] = true;
      ccCount++;
      lastCC = cc;
      lastVal = val;
    }
    if (cc == CC_PLAY && val > 0) params.gate = true;
    if (cc == CC_STOP && val > 0) params.gate = false;
  }
}

// ---- USB audio endpoint accounting ----
static uint32_t g_tx_done = 0, g_tx_bytes = 0;
extern "C" bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent,
                                      uint8_t func_id, uint8_t ep_in,
                                      uint8_t cur_alt_setting) {
  (void)rhport; (void)func_id; (void)ep_in; (void)cur_alt_setting;
  ++g_tx_done;
  g_tx_bytes += n_bytes_sent;
  return true;
}

static void report() {
  static uint32_t prev_ms = 0, prev_done = 0, prev_bytes = 0;
  const uint32_t now = millis();
  if (now - prev_ms < 2000u) return;  // every 2 s
  const uint32_t dt = now - prev_ms;
  const uint32_t xfers = g_tx_done - prev_done;
  const uint32_t bytes = g_tx_bytes - prev_bytes;
  tu_fifo_t *ff = tud_audio_get_ep_in_ff();
  Serial1.print("[diag] xfer/s=");
  Serial1.print((uint32_t)((uint64_t)xfers * 1000ull / dt));
  Serial1.print(" smp/s=");
  Serial1.print((uint32_t)((uint64_t)bytes * 1000ull / dt / 2));
  Serial1.print(" fifo=");
  Serial1.print(ff ? tu_fifo_count(ff) : 0);
  Serial1.print('/');
  Serial1.print(ff ? ff->depth : 0);
  Serial1.print(" pend=");
  Serial1.print(bridge.pending());
  Serial1.print(" und=");
  Serial1.print(bridge.underruns());
  Serial1.print(" cc=");
  Serial1.print(ccCount);
  Serial1.print(" last=");
  Serial1.print(lastCC);
  Serial1.print('/');
  Serial1.print(lastVal);
  Serial1.print(" dev=");
  Serial1.print(tuh_mounted(1) ? 1 : 0);
  Serial1.print(" midi=");
  Serial1.print(tuh_midi_mounted(0) ? 1 : 0);
  Serial1.print(" gate=");
  Serial1.print(params.gate ? 1 : 0);
  Serial1.print(" play=");
  Serial1.print(envelope.playing() ? 1 : 0);
  Serial1.print(" env=");
  Serial1.print(envGain);
  Serial1.print(" out=");
  Serial1.println(lastOut);
  prev_ms = now;
  prev_done = g_tx_done;
  prev_bytes = g_tx_bytes;
}

void setup() {
  // Console FIRST (pre-begin Mbed UART writes HardFault — see plan file).
  Serial1.begin(ARDUINO_TINYUSB_BAUD);
  Serial1.println("[MozziGIGA] synth+UAC2+MIDI boot");

  // UAC2 capture device on Type-C (kills Mbed CDC by design).
  tud_arduino_init();
  // MIDI host on Type-A (VBUS + settle handled in BSP).
  tuh_arduino_init();

  bridge.begin();

  // Audible defaults until the nanoK is moved (cache starts zeroed).
  const uint8_t defF[8] = {110, 60, 30, 40, 90, 40, 30, 100};
  const uint8_t defK[8] = {64, 10, 64, 40, 40, 0, 0, 0};
  for (uint8_t i = 0; i < 8; i++) {
    ccCache[CC_FADER[i]] = defF[i];
    ccCache[CC_KNOB[i]] = defK[i];
  }

  startMozzi(MOZZI_CONTROL_RATE);
  aSaw.setFreq(110);
  aSquare.setFreq(111);
  kLfo.setFreq(5.0f);
  lpf.setCutoffFreqAndResonance(128, 40);
  envelope.setADLevels(200, 120);
  envelope.setTimes(20, 200, 800, 300);
  envelope.noteOn();
}

void loop() {
  tud_arduino_task();
  tuh_arduino_poll();
  report();

  // Top up the synth ring (one sample per audioHook, bounded by ring room).
  while (bridge.canPush()) audioHook();

  if (!tud_audio_mounted()) return;

  // Gate on FIFO room: writes into a full FIFO silently overwrite (no back-pressure).
  tu_fifo_t *ff = tud_audio_get_ep_in_ff();
  if (!ff) return;
  while (tu_fifo_count(ff) <= (uint16_t)(ff->depth - Uac2Bridge::kUsbEpBytes)) {
    bridge.fill(chunk, kChunkSamples);
    tud_audio_write(chunk, sizeof(chunk));
  }
}
