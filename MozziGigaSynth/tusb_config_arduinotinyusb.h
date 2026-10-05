// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com
// Part of Mozzi-tinyusb. See LICENSE at the repository root.

// tusb_config_arduinotinyusb.h — UAC2 device (rhport 0, Type-C) + MIDI host
// (rhport 1, Type-A) simultaneously. GIGA's two controllers are independent,
// which is the only configuration where TinyUSB can be a device and a host at
// once.
//
// The filename is load-bearing, and is not this project's choice: it is the name
// of the library's *active config slot* (src/tusb_config_arduinotinyusb.h), the
// file TinyUSB actually reads. Two consumers, one file:
//   * arduino-cli — scripts/build.sh passes
//     -DARDUINO_TINYUSB_CONFIG_FILE="<sketch>/tusb_config_arduinotinyusb.h"
//   * Arduino IDE — no -D is possible, so this file is copied over the library's
//     active slot instead. scripts/ide_setup.sh <SketchName> does that.
//
// This sketch's config is the one that most needs the per-sketch swap: unlike
// the other three, which are device-only, it enables CFG_TUH_ENABLED. Copying
// another sketch's config in its place compiles the host side out, and the
// failure is a link error naming the missing tuh_* call — loud, but only after
// you have already rebuilt the sketch you were working on.
//
// Note the include guard is distinct from the other sketches'. Every sketch in
// this project wraps its own config in MOZZI_USB_TUSB_CONFIG_H, which is
// harmless while exactly one config is compiled per build — but this file differs
// from the others (it enables CFG_TUH_ENABLED and CFG_TUH_MIDI), so a collision
// would silently disable the MIDI host. Distinct guards make that impossible.
//
// CFG_TUSB_DEBUG stays 0: ISR-context logging starves isochronous audio. The
// shared config defaulted this to 2 until TinyUSB_Arduino f8927bc, so this line
// is redundant with the library default now and is kept deliberately — the pin
// is a SHA of an experimental repo, and 2 fails as corruption, not as an error.

#ifndef MOZZI_GIGA_TUSB_CONFIG_H
#define MOZZI_GIGA_TUSB_CONFIG_H

#define CFG_TUSB_DEBUG 0

#define CFG_TUD_ENABLED 1
#define CFG_TUH_ENABLED 1
#define CFG_TUD_CDC     0
#define CFG_TUD_AUDIO   1
#define CFG_TUH_MIDI    1

#include "config/tusb_config_common.h"

// 4 x 98 B = 4 ms of slack in the device->host audio FIFO, restated so this file
// pins it rather than inheriting it. 4x not 8x: TinyUSB reads this FIFO's
// occupancy to pick between its 94/96/98-byte clock-deviation packets, and a
// deeper buffer skews that decision. See docs/dsp-notes.md.
#undef CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ (4 * CFG_TUD_AUDIO_EP_SZ_IN)

#endif  // MOZZI_GIGA_TUSB_CONFIG_H
