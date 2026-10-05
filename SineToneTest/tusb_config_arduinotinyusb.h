// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com
// Part of Mozzi-tinyusb. See LICENSE at the repository root.

// tusb_config_arduinotinyusb.h — per-sketch TinyUSB config for the Mozzi USB
// synth.
//
// The filename is load-bearing, and is not this project's choice: it is the name
// of the library's *active config slot* (src/tusb_config_arduinotinyusb.h), the
// file TinyUSB actually reads. Two consumers, one file:
//   * arduino-cli — scripts/build.sh passes
//     -DARDUINO_TINYUSB_CONFIG_FILE="<sketch>/tusb_config_arduinotinyusb.h"
//   * Arduino IDE — no -D is possible, so this file is copied over the library's
//     active slot instead. scripts/ide_setup.sh <SketchName> does that.
//
// It cannot be an #include of a differently-named file: once copied into the
// library's src/, a quoted #include resolves against that directory and picks up
// the library's own headers rather than the sketch's. TinyUSB_Arduino's README
// gives the same reason for the copy-over being a copy at all.
//
// Two things matter in the content. The first is now also the library default;
// the second is not, and is the reason this file still earns its place:
//
//  1. CFG_TUSB_DEBUG must be 0. At 2, logging runs from inside the USB ISR at
//     115200 baud: ~3.5 ms of blocking UART per line, longer than the whole 1 ms
//     USB audio frame. The library's own README says as much and flags GCM4
//     device panics and an ESP32-S3 interrupt-watchdog reboot.
//
//     The shared config (src/config/tusb_config_common.h) defaulted this to 2
//     until TinyUSB_Arduino f8927bc, which lowered it to the upstream TinyUSB
//     default of 0. So this line is redundant with the library default now, and
//     is kept anyway: the pin is a SHA of a repo its author calls experimental,
//     and inheriting 2 breaks audio as corruption rather than as an error. The
//     #ifndef in the shared header lets this file win either way, so the IDE
//     path is safe too and not just scripts/build.sh.
//
//  2. The audio EP IN software FIFO stays at 4 x 98 B = 4 ms. It is restated
//     explicitly below even though the shared config already sets exactly this,
//     so a future bump cannot silently change it. 4x is deliberate: TinyUSB
//     reads this FIFO's occupancy to choose between its 94/96/98-byte
//     clock-deviation packets, and a deeper buffer skews that decision.
//     Measured steady at 196/392 while capturing. See docs/dsp-notes.md.

#ifndef MOZZI_USB_TUSB_CONFIG_H
#define MOZZI_USB_TUSB_CONFIG_H

#define CFG_TUSB_DEBUG 0

#define CFG_TUD_ENABLED 1
#define CFG_TUH_ENABLED 0
#define CFG_TUD_CDC     0
#define CFG_TUD_AUDIO   1

#include "config/tusb_config_common.h"

// Restate the shared default so this file pins it rather than inheriting it.
// 4x, not 8x: TinyUSB reads this FIFO's occupancy to pick between its
// 94/96/98-byte clock-deviation packets, and a deeper buffer skews that decision.
#undef CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ (4 * CFG_TUD_AUDIO_EP_SZ_IN)

#endif  // MOZZI_USB_TUSB_CONFIG_H
