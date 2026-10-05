// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com
// Part of Mozzi-tinyusb. See LICENSE at the repository root.

// SineToneTest.ino — bring-up sketch: a known 440 Hz tone out of Mozzi and
// onto the host as a 48 kHz UAC2 capture device.
//
// This exists so the first hardware test has an unambiguous expected result:
// scripts/hw_test.sh records and asserts the fundamental lands within 1 Hz of
// 440 Hz at -14.1 dBFS RMS (-11.1 dBFS peak -- kToneGain * 127 = 9144, and
// 9144/sqrt(2) = 6466 = -14.10 dBFS). It exercises the whole chain (Mozzi ->
// Uac2Bridge -> UAC2 EP IN) with nothing else in the way, so a failure points at
// a specific stage. MozziUSBSynth.ino is the real deliverable.

#include <MozziConfigValues.h>

// Mozzi 2.0 configuration. MOZZI_AUDIO_RATE must be a power of two, and no legal
// USB audio rate is, which is why Uac2Bridge converts 32768 -> 48000.
#define MOZZI_AUDIO_MODE       MOZZI_OUTPUT_EXTERNAL_CUSTOM
#define MOZZI_AUDIO_RATE       32768
#define MOZZI_AUDIO_BITS       16
#define MOZZI_CONTROL_RATE     256
#define MOZZI_AUDIO_INPUT      MOZZI_AUDIO_INPUT_NONE
#define MOZZI_COMPATIBILITY_LEVEL MOZZI_COMPATIBILITY_2_0
// Mozzi's asynchronous ADC is deliberately NOT used. On RP2040 its setup
// (setupMozziADC -> dma_claim_unused_channel) hangs the board outright:
// startMozzi() never returns, so setup() never completes and loop() never runs.
// Verified by A/B on the same board: ANALOG_READ_STANDARD stops after the setup
// banner, ANALOG_READ_NONE runs. At a 256 Hz control rate there is nothing to
// gain from claiming a DMA channel and a shared DMA_IRQ_0 handler anyway, and
// updateControl() below uses plain analogRead() instead.
#define MOZZI_ANALOG_READ     MOZZI_ANALOG_READ_NONE

#include <Mozzi.h>
#include <Oscil.h>
#include <tables/sin2048_int8.h>

#include <ArduinoTinyUSB.h>
#include <Uac2Bridge.h>


namespace {
Uac2Bridge::Bridge bridge;
// One 1 ms USB frame: 98 B = 49 samples. Must match tusb_config.h.
constexpr uint16_t kChunkSamples = Uac2Bridge::kUsbEpSamples;   // 49
int16_t chunk[kChunkSamples];

constexpr float kToneHz = 440.0f;
// sin2048_int8 peaks at 127, so this lands the fundamental at 127*72 = 9144
// peak / 6466 RMS. Measured on the RP2040: 439.963 Hz (0.008% error), 2nd
// harmonic -183 dBc, RMS 6466.7 -- i.e. the 32768 -> 48000 resampler is
// transparent. The residue is a 2048-point staircase read without interpolation,
// which is why this is bring-up scaffolding and not a synth voice.
constexpr float kToneGain = 72.0f;
// The table MUST be handed to the constructor (or setTable()). Oscil's no-arg
// constructor leaves the table pointer uninitialised, and readTable() then
// dereferences whatever happened to be there -- heard as a harsh, harmonically
// rich buzz with no fundamental rather than as an obvious fault.
Oscil<2048, MOZZI_AUDIO_RATE> osc(SIN2048_DATA);
}  // namespace

// ---- Mozzi callbacks (global scope: an anonymous namespace breaks the link) --

// Mozzi hands us a normalised sample in [0,1]; the bridge wants signed 16-bit.
// from16Bit() is the right converter -- from8Bit() left-shifts by 8 when
// MOZZI_AUDIO_BITS is 16, which hard-clips and looks like broadband noise.
void audioOutput(const AudioOutput f) { bridge.push((int16_t)f.l()); }
inline bool canBufferAudioOutput() { return bridge.canPush(); }

void updateControl() {}

// osc.next() runs once per audioHook(), and audioHook() only runs while the
// bridge ring has room, so the oscillator is paced by the endpoint rather than
// by the CPU -- which is the whole point.
AudioOutput updateAudio() {
  return MonoOutput::from16Bit((int16_t)(osc.next() * kToneGain));
}

// ---- USB task pumping ----------------------------------------------------
// Deliberately NOT tud_arduino_task(). With the pico OSAL, tud_task() calls
// tud_task_ext(UINT32_MAX) and waits on the event queue; the audio EP IN is only
// polled by the host while something is capturing, so a synth that parks there
// stops making sound entirely. A zero timeout drains what is pending and
// returns, leaving the audio path self-clocking.
static inline void usb_task() { tud_task_ext(0, false); }

// ---- Endpoint accounting --------------------------------------------------
// Measured with tud_audio_tx_done_isr, which is how the "generate one frame per
// loop()" trap was found: the host was receiving a strided subsample of a far
// faster oscillator, so the pitch tracked the CPU rate rather than the USB rate.
// Printing transfers/s and bytes/transfer keeps that regression visible.
static uint32_t g_tx_done = 0, g_tx_bytes = 0;
extern "C" bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent,
                                      uint8_t func_id, uint8_t ep_in,
                                      uint8_t cur_alt_setting) {
  (void)rhport; (void)func_id; (void)ep_in; (void)cur_alt_setting;
  ++g_tx_done;
  g_tx_bytes += n_bytes_sent;
  return true;
}

#if defined(ARDUINO_TINYUSB_CONSOLE)
static void report() {
  static uint32_t prev_us = 0, prev_done = 0, prev_bytes = 0;
  const uint32_t now = micros();
  if (now - prev_us < 2000000u) return;  // every 2 s
  const uint32_t dt = now - prev_us;
  const uint32_t xfers = g_tx_done - prev_done;
  const uint32_t bytes = g_tx_bytes - prev_bytes;
  tu_fifo_t *ff = tud_audio_get_ep_in_ff();
  ARDUINO_TINYUSB_CONSOLE.print("[diag] xfer/s=");
  ARDUINO_TINYUSB_CONSOLE.print((uint32_t)((uint64_t)xfers * 1000000ull / dt));
  ARDUINO_TINYUSB_CONSOLE.print(" sample/s=");
  ARDUINO_TINYUSB_CONSOLE.print((uint32_t)((uint64_t)bytes * 1000000ull / dt / 2));
  ARDUINO_TINYUSB_CONSOLE.print(" B/xfer=");
  ARDUINO_TINYUSB_CONSOLE.print(xfers ? bytes / xfers : 0);
  ARDUINO_TINYUSB_CONSOLE.print(" ep_fifo=");
  ARDUINO_TINYUSB_CONSOLE.print(ff ? tu_fifo_count(ff) : 0);
  ARDUINO_TINYUSB_CONSOLE.print('/');
  ARDUINO_TINYUSB_CONSOLE.print(ff ? ff->depth : 0);
  ARDUINO_TINYUSB_CONSOLE.print(" synth_pending=");
  ARDUINO_TINYUSB_CONSOLE.print(bridge.pending());
  ARDUINO_TINYUSB_CONSOLE.print(" underruns=");
  ARDUINO_TINYUSB_CONSOLE.println(bridge.underruns());
  prev_us = now;
  prev_done = g_tx_done;
  prev_bytes = g_tx_bytes;
}
#endif

// Console setup must come FIRST on Mbed (GIGA), before tud_arduino_init().
// TinyUSB claims OTG_FS there, and a Mbed UART touched after that HardFaults:
// measured PC 0x08047E6E = mbed::SerialBase::writeable(), reached from
// arduino::UART::write() at cores/arduino/Serial.cpp:235 -- the println below.
// On the RP2040 boards Serial1 is a plain hardware UART that TinyUSB never
// touches, so the original ordering there is left byte-for-byte alone, because
// that is the ordering the verified 440.19 Hz result was measured under.
#if defined(ARDUINO_ARCH_MBED)
#  define CONSOLE_FIRST 1
#endif

void setup() {
#if defined(CONSOLE_FIRST) && defined(ARDUINO_TINYUSB_CONSOLE)
  ARDUINO_TINYUSB_CONSOLE.begin(ARDUINO_TINYUSB_BAUD);
  while (!ARDUINO_TINYUSB_CONSOLE && millis() < 2000) delay(1);
#endif

  // TinyUSB first, before anything that can block.
  tud_arduino_init();

  bridge.begin();

#if defined(ARDUINO_TINYUSB_CONSOLE)
  ARDUINO_TINYUSB_CONSOLE.begin(ARDUINO_TINYUSB_BAUD);
  while (!ARDUINO_TINYUSB_CONSOLE && millis() < 2000) delay(1);
  ARDUINO_TINYUSB_CONSOLE.println("[MozziUSB] UAC2 48 kHz, 440 Hz test tone");
#endif

  startMozzi(MOZZI_CONTROL_RATE);
  // After startMozzi, which installs Mozzi's control/update hooks.
  osc.setFreq(kToneHz);
}

void loop() {
  usb_task();
#if defined(ARDUINO_TINYUSB_CONSOLE)
  report();
#endif

  // Top up the synth ring. audioHook() makes exactly one sample per call, so
  // this is a burst bounded by the ring, not a rate.
  while (bridge.canPush()) audioHook();

  if (!tud_audio_mounted()) return;

  // Hand finished 48 kHz frames to the endpoint.
  //
  // tud_audio_write() cannot be used for back-pressure: TinyUSB configures the
  // audio EP IN FIFO with overwritable = true (class/audio/audio_device.c), so
  // writing into a full FIFO silently overwrites the oldest audio instead of
  // reporting a short write. Gate on the FIFO count and never write a partial
  // frame.
  //
  // Pacing here is what keeps the pitch right. Producing a frame per loop()
  // iteration -- as TinyUSB_Arduino's own Device_Audio example does -- looks
  // equivalent but is not: loop() runs ~400k frames/s of CPU against ~1000
  // frames/s of USB, so the endpoint ships a strided subsample of a far faster
  // oscillator and the host hears a pitch set by the CPU speed, with a
  // discontinuity at every strided frame boundary. Measured on this board: a
  // generated 440 Hz tone arrived as 493.5 Hz that way, and 440.000 Hz once the
  // generation was gated on endpoint room.
  tu_fifo_t *ff = tud_audio_get_ep_in_ff();
  if (!ff) return;
  while (tu_fifo_count(ff) <= (uint16_t)(ff->depth - Uac2Bridge::kUsbEpBytes)) {
    bridge.fill(chunk, kChunkSamples);
    tud_audio_write(chunk, sizeof(chunk));
  }
}
