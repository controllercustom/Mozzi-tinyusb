// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com
// Part of Mozzi-tinyusb. See LICENSE at the repository root.

// MozziUSBSynth.ino — a monophonic subtractive Mozzi synth whose audio leaves
// the board over USB as a 48 kHz mono UAC2 capture device.
//
// Chain: updateAudio() (Mozzi, 32768 Hz) -> Uac2Bridge ring -> 32768->48000
// resample -> 98-byte UAC2 EP IN chunks -> snd-usb-audio on the host.
//
// The host sees a capture device, so the synth can be recorded or monitored into
// a DAW. It is not a low-latency instrument: ~8 ms of synth ring + 4 ms of USB
// FIFO sit in front of the audio, before any host-side buffering.
//
// Controls (Pico analog pins, 12-bit):
//   A0  filter cutoff   80 Hz .. 12 kHz
//   A1  oscillator 2 detune / mix   0 .. +30 cents
//   A2  master level    0 .. 0.9 FS
// GP15 button          pluck (re-triggers the envelope); with no button, the
//                      envelope loops so the board makes sound on its own.
//
// Bare board with nothing wired to A0..A2 or GP15: uncomment both lines.
// (The scripted build passes these as -D flags instead — same effect.)
#ifndef NO_POTS
// #define NO_POTS   // fixed settings instead of reading the pots
#endif
#ifndef NO_BUTTON
// #define NO_BUTTON // retrigger the envelope every control tick
#endif

#include <MozziConfigValues.h>

// MOZZI_AUDIO_RATE must be a power of two (Mozzi asserts it) and no legal USB
// audio rate is, which is why Uac2Bridge converts 32768 -> 48000.
#define MOZZI_AUDIO_MODE       MOZZI_OUTPUT_EXTERNAL_CUSTOM
#define MOZZI_AUDIO_RATE       32768
#define MOZZI_AUDIO_BITS       16
#define MOZZI_CONTROL_RATE     256
#define MOZZI_AUDIO_INPUT      MOZZI_AUDIO_INPUT_NONE
#define MOZZI_COMPATIBILITY_LEVEL MOZZI_COMPATIBILITY_2_0
// Only the RP2040 and Renesas ports implement Mozzi's asynchronous ADC (SAMD21
// and mbed reject it outright), and it would claim a DMA channel plus a shared
// DMA_IRQ_0 handler for pot reads that only happen 256 times a second.
#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_FSP)
  #define MOZZI_ANALOG_READ   MOZZI_ANALOG_READ_STANDARD
#else
  #define MOZZI_ANALOG_READ   MOZZI_ANALOG_READ_NONE
#endif

#include <Mozzi.h>
#include <Oscil.h>
#include <StateVariable.h>
#include <DCfilter.h>
#include <ADSR.h>
#include <tables/sin2048_int8.h>

#include <ArduinoTinyUSB.h>
#include <Uac2Bridge.h>

namespace {

Uac2Bridge::Bridge bridge;
constexpr uint16_t kChunkSamples = Uac2Bridge::kUsbEpSamples;  // 49 = 98 B
int16_t chunk[kChunkSamples];

// ---- voice -------------------------------------------------------------
// Oscil's second template parameter is the update rate it is optimised for, so
// the audio-rate oscillator must be MOZZI_AUDIO_RATE (the control-rate one is
// MOZZI_CONTROL_RATE).
Oscil<2048, MOZZI_AUDIO_RATE> oscA(SIN2048_DATA);
Oscil<2048, MOZZI_AUDIO_RATE> oscB(SIN2048_DATA);
Oscil<2048, MOZZI_CONTROL_RATE> lfoShape(SIN2048_DATA);
StateVariable<LOWPASS> filter;
// The StateVariable filter's integrators leave a small DC offset, which showed
// up as a -22.9 dBc spur at 0 Hz and wasted headroom. Strip it.
DCfilter dcfilter(0.995f);
ADSR<MOZZI_CONTROL_RATE, MOZZI_CONTROL_RATE> envelope;

constexpr float kBaseHz = 110.0f;
constexpr int kPinCutoff = A0;
constexpr int kPinDetune = A1;
constexpr int kPinLevel = A2;
constexpr int kPinButton = 15;

unsigned int cutoff_hz = 900;
float detune_cents = 0.0f;
int level_scale = 200;      // 0..~0.78 of full scale
int env = 0;                // ADSR level, updated at the control rate
bool button_last = false;
bool have_note = false;

uint32_t underruns_reported = 0;

inline int pot_read(int pin, int hi) {
#if MOZZI_IS(MOZZI_ANALOG_READ, MOZZI_ANALOG_READ_NONE)
  // Pico's analogRead() is 12-bit; ask Mozzi's portable path for 10 bits where
  // the async reader exists, and scale hardware-native readings to 10 bits.
  return map(analogRead(pin), 0, 4095, 0, 1023);
#else
  return map(mozziAnalogRead<10>(pin), 0, 1023, 0, 1023);
#endif
}

}  // namespace

// ---- Mozzi's external-output hooks (EXTERNAL_CUSTOM) --------------------
// MUST be at global scope: MozziPrivate::audioHook() calls them unqualified, so
// a definition in an anonymous namespace would not link.
//
// Everything runs in loop() context: no timer ISR, no DMA, no shared IRQ. That
// is what EXTERNAL_CUSTOM buys, and it is why there is no ISR-context logging
// hazard on this path at all.
void audioOutput(const AudioOutput f) { bridge.push((int16_t)f.l()); }
inline bool canBufferAudioOutput() { return bridge.canPush(); }

void updateControl() {
#if defined(NO_POTS)
  // Bench build: nothing is wired to A0..A2, so analogRead() returns a
  // floating value and the voice has no defined pitch or level. Use fixed
  // settings instead so the sketch is testable on a bare board.
  const unsigned int cutoff_hz = 2000;
  const float detune_cents = 7.0f;
  const int level_scale = 200;
#else
  cutoff_hz = (unsigned int)map(pot_read(kPinCutoff, 10), 0, 1023, 80, 12000);
  detune_cents = map(pot_read(kPinDetune, 10), 0, 1023, 0, 300) / 100.0f;
  level_scale = map(pot_read(kPinLevel, 10), 0, 1023, 0, 230);
#endif

  filter.setCentreFreq(cutoff_hz);
  filter.setResonance(96);   // Q0n8: higher = less resonance

  oscA.setFreq(kBaseHz);
  oscB.setFreq(kBaseHz * powf(2.0f, detune_cents / 1200.0f));
  lfoShape.setFreq(0.7f);

  // Note handling: a button plucks; with no button the envelope loops so the
  // board makes sound as soon as it is plugged in.
  const bool pressed = digitalRead(kPinButton) == LOW;
  const bool pressed_edge = pressed && !button_last;
  button_last = pressed;
#if defined(NO_BUTTON)
  if (true) {
#else
  if (pressed_edge) {
#endif
    if (have_note) envelope.noteOff();
    envelope.noteOn();
    have_note = true;
  }

  // The ADSR is stepped entirely at the control rate (the documented cheap
  // path); updateAudio() just reads the cached level.
  envelope.update();
  env = envelope.next();
}

AudioOutput updateAudio() {
  int v = (oscA.next() + oscB.next()) >> 1;          // 8-bit oscillators
  v = filter.next(v);                                 // filter works in 8-bit
  v = dcfilter.next(v);                               // strip integrator offset

  // Promote to 16-bit BEFORE the amplitude scalings. Chaining three 8-bit
  // stages with a >>8 each costs ~48 dB of headroom and put the whole voice at
  // about -48 dBFS RMS, i.e. inaudible on a bench.
  int32_t a = (int32_t)v * 256;                       // -> +/-32k
  const int trem = (lfoShape.next() >> 6) + 128;      // 8-bit LFO -> 0..255
  a = (a * (trem + 64)) >> 8;                         // +/-25% vibrato
  a = (a * (env & 0xFF)) >> 8;                        // envelope
  a = (a * level_scale) >> 8;                         // level
  return MonoOutput::from16Bit((int16_t)a).clip();
}


// ---- USB task pumping ----------------------------------------------------
// Deliberately NOT tud_arduino_task(). With the pico OSAL, tud_task() calls
// tud_task_ext(UINT32_MAX) and blocks on a semaphore until a USB event arrives.
// A UAC2 isochronous IN endpoint is only polled by the host while something is
// actually capturing, so with nothing recording there may be no events at all -
// and a synth that parks in tud_task() stops synthesising entirely. Observed as
// bridge.produced() frozen at 784 samples (16 ms) while the host recorded
// garbage. A zero timeout processes whatever is pending and returns, so the
// audio path stays self-clocking and the FIFO simply holds the newest audio.
static inline void usb_task() { tud_task_ext(0, false); }

// Console setup must come FIRST on Mbed (GIGA), before tud_arduino_init().
// TinyUSB claims OTG_FS there, and a Mbed UART touched after that HardFaults in
// mbed::SerialBase::writeable(). See SineToneTest.ino for the measurement. On the
// RP2040 boards Serial1 is a plain hardware UART that TinyUSB never touches, so
// the original ordering there is left alone — it is what the verified result was
// measured under.
#if defined(ARDUINO_ARCH_MBED)
#  define CONSOLE_FIRST 1
#endif

void setup() {
  pinMode(kPinButton, INPUT_PULLUP);

#if defined(CONSOLE_FIRST) && defined(ARDUINO_TINYUSB_CONSOLE)
  ARDUINO_TINYUSB_CONSOLE.begin(ARDUINO_TINYUSB_BAUD);
  while (!ARDUINO_TINYUSB_CONSOLE && millis() < 2000) delay(1);
#endif

  // TinyUSB before anything that can block.
  tud_arduino_init();

  bridge.begin();
  envelope.setLevels(255, 200, 140, 0);
  envelope.setTimes(8, 120, 0, 180);
  envelope.noteOff();  // start released, so the first pluck is clean

#if defined(ARDUINO_TINYUSB_CONSOLE)
  ARDUINO_TINYUSB_CONSOLE.begin(ARDUINO_TINYUSB_BAUD);
  while (!ARDUINO_TINYUSB_CONSOLE && millis() < 2000) delay(1);
  ARDUINO_TINYUSB_CONSOLE.println("[MozziUSB] UAC2 48 kHz mono, 110 Hz subtractive");
  ARDUINO_TINYUSB_CONSOLE.println("[MozziUSB] A0 cutoff  A1 detune  A2 level  "
                                  "GP15 pluck");
#endif

  startMozzi(MOZZI_CONTROL_RATE);
}

void loop() {
  usb_task();

  // Stage 1: top up the synth ring. audioHook() yields exactly one sample.
  while (bridge.canPush()) audioHook();

  if (!tud_audio_mounted()) return;

  // Stage 2: hand finished 1 ms frames to the UAC2 endpoint.
  //
  // tud_audio_write() gives no back-pressure: TinyUSB marks the audio EP IN FIFO
  // overwritable, so writing when it is full overwrites the oldest audio instead
  // of returning a short count. Size the write from the FIFO occupancy instead,
  // and never hand over a partial frame.
  tu_fifo_t *ff = tud_audio_get_ep_in_ff();
  if (!ff) return;
  while (tu_fifo_count(ff) <= (uint16_t)(ff->depth - Uac2Bridge::kUsbEpBytes)) {
    const uint32_t under = bridge.fill(chunk, kChunkSamples);
    tud_audio_write(chunk, sizeof(chunk));

    // An underrun means the synth could not keep the endpoint fed: the host gets
    // a silent millisecond. Report it, rate-limited, so glitches are visible
    // rather than mysterious.
    if (under && underruns_reported < 1000) {
      ++underruns_reported;
#if defined(ARDUINO_TINYUSB_CONSOLE)
      if (underruns_reported < 5 || (underruns_reported % 100) == 0) {
        ARDUINO_TINYUSB_CONSOLE.print("[MozziUSB] underruns: ");
        ARDUINO_TINYUSB_CONSOLE.println(bridge.underruns());
      }
#endif
    }
  }
}
