// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com
// Part of Mozzi-tinyusb. See LICENSE at the repository root.

// PatternTest.ino — TRANSPORT-ONLY diagnostic. No Mozzi, no bridge.
//
// Feeds a sequence of trivially recognisable 1 ms patterns into the UAC2 EP IN
// and writes each pattern's name to the console, so one 6 s capture can be
// matched block-for-block against the device's own log. If the blocks do not
// line up, the fault is in the USB audio path; if they do, the fault is
// downstream (Mozzi / Uac2Bridge).
//
//   block 0 (1 s): 0x0000        silence
//   block 1 (1 s): 0x7FFF        +full-scale DC
//   block 2 (1 s): 0x0001        +1 LSB DC
//   block 3 (1 s): 0x5A5A        alternating bytes (a square at 12 kHz)
//   block 4 (2 s): 440.0 Hz sine at -6.0 dBFS, exact 32.32 phase accumulator
//   block 6+     : 0x0000        silence (tail marker)
//
// Blocks 0-3 prove sample integrity and ordering. Block 4 proves *timing*,
// which constant patterns cannot: if the reconstructed fundamental is not
// 440.0 Hz the link is time-warping the stream, and a constant capture would
// still have looked perfect.

#include <ArduinoTinyUSB.h>

namespace {
constexpr uint16_t kFrameSamples = 49;  // 98 B = one 1 ms full-speed frame @ 48 kHz
int16_t frame[kFrameSamples];
constexpr uint16_t kFrameBytes = kFrameSamples * 2;
uint32_t block = 0;
uint32_t block_start_us = 0;
constexpr float kSineHz = 440.0f;
constexpr uint16_t kSineAmp = 16384;  // -6.02 dBFS
uint32_t phase = 0;
// 32.32 fixed point so the increment is exact: 440/48000 * 2^32 = 39426470.4
constexpr uint32_t kPhaseInc = (uint32_t)((440ull << 32) / 48000ull);

// Ground-truth accounting of what the device actually hands to the USB
// controller. If this reports ~98,000 B/s the device is sending full 98-byte
// frames at 1 kHz and any pitch error is the host's; if it reports less, the
// device is short-changing the endpoint.
uint32_t tx_done = 0, tx_bytes = 0, tx_prev = 0, txb_prev = 0;
uint32_t tx_report_us = 0;
}  // namespace

extern "C" bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent,
                                      uint8_t func_id, uint8_t ep_in,
                                      uint8_t cur_alt_setting) {
  (void)rhport; (void)func_id; (void)ep_in; (void)cur_alt_setting;
  tx_done++;
  tx_bytes += n_bytes_sent;
  return true;
}

static uint16_t pattern_for(uint32_t b) {
  switch (b) {
    case 0: return 0x0000;
    case 1: return 0x7FFF;
    case 2: return 0x0001;
    case 3: return 0x5A5A;
    default: return 0x0000;
  }
}

void setup() {
  ARDUINO_TINYUSB_CONSOLE.begin(ARDUINO_TINYUSB_BAUD);
  tud_arduino_init();
  while (!ARDUINO_TINYUSB_CONSOLE && millis() < 2000) {
    delay(1);
  }
  block_start_us = micros();
  ARDUINO_TINYUSB_CONSOLE.println("[pat] ready");
}

void loop() {
  tud_arduino_task();
  if (!tud_audio_mounted()) return;

  const uint32_t now = micros();
  if (now - tx_report_us >= 1000000u) {
#if defined(ARDUINO_TINYUSB_CONSOLE)
    const uint32_t dt = now - tx_report_us;
    ARDUINO_TINYUSB_CONSOLE.print("[tx] xfers/s=");
    ARDUINO_TINYUSB_CONSOLE.print((uint32_t)(tx_done - tx_prev) * 1000000u / dt);
    ARDUINO_TINYUSB_CONSOLE.print(" bytes/s=");
    ARDUINO_TINYUSB_CONSOLE.print((uint32_t)(tx_bytes - txb_prev) * 1000000u / dt);
    ARDUINO_TINYUSB_CONSOLE.print(" B/xfer=");
    const uint32_t d = tx_done - tx_prev;
    ARDUINO_TINYUSB_CONSOLE.println(d ? (uint32_t)(tx_bytes - txb_prev) / d : 0);
#endif
    tx_prev = tx_done;
    txb_prev = tx_bytes;
    tx_report_us = now;
  }
  if (now - block_start_us >= 1000000u) {
    block++;
    block_start_us = now;
#if defined(ARDUINO_TINYUSB_CONSOLE)
    ARDUINO_TINYUSB_CONSOLE.print("[pat] block=");
    ARDUINO_TINYUSB_CONSOLE.print(block);
    ARDUINO_TINYUSB_CONSOLE.print(" pat=");
    ARDUINO_TINYUSB_CONSOLE.println((block >= 4 && block < 6) ? 0x5A5E : pattern_for(block), HEX);
#endif
  }

  // PACE THE GENERATOR TO THE ENDPOINT.
  //
  // Generating one frame per loop() iteration is a trap: loop() runs orders of
  // magnitude faster than the endpoint drains (measured ~400k frames/s of CPU
  // against ~1000 frames/s of USB), so the frames the endpoint actually
  // transmits are a strided subsample of a far faster oscillator. The tone then
  // arrives at the wrong pitch (loop-rate dependent, hence never reproducible)
  // with a discontinuity at every strided frame boundary. Audio is only
  // generated here, for a frame that is about to be sent, and never faster than
  // the endpoint has room for.
  tu_fifo_t *ff = tud_audio_get_ep_in_ff();
  if (!ff) return;
  while (tu_fifo_count(ff) <= (uint16_t)(ff->depth - kFrameBytes)) {
    for (uint16_t i = 0; i < kFrameSamples; i++) {
      if (block >= 4 && block < 6) {
        phase += kPhaseInc;
        const float a = (float)phase * (1.0f / 4294967296.0f) * 6.2831853f;
        frame[i] = (int16_t)(sinf(a) * (float)kSineAmp);
      } else {
        frame[i] = (int16_t)pattern_for(block);
      }
    }
    tud_audio_write(frame, sizeof(frame));
  }
}
