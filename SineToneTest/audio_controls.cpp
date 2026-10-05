// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com
// Part of Mozzi-tinyusb. See LICENSE at the repository root.

// audio_controls.cpp — UAC2 entity CUR/RANGE handlers for the Mozzi USB synth.
//
// The TinyUSB audio HOST probes Clock Source sample-frequency RANGE and
// Feature Unit volume RANGE during mount; the weak defaults stall, so the
// host never mounts (both directions failed identically). This implements
// the app callbacks for our fixed 48 kHz mono mic topology:
// Clock 0x04, Input Term 0x01, FU 0x02 (mute+volume), Output Term 0x03.

#include "ArduinoTinyUSB.h"

static uint32_t audio_samp_freq = 48000;
static uint8_t  audio_clk_valid = 1;
static uint8_t  audio_mute[2]   = {0, 0};
static int16_t  audio_volume[2] = {0, 0}; // 0 dB both channels

extern "C" bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  uint8_t ch      = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t entity  = TU_U16_HIGH(p_request->wIndex);
  if (ch > 1) ch = 1;

  if (entity == 4) { // Clock Source
    if (ctrlSel == AUDIO20_CS_CTRL_SAM_FREQ) {
      if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
        return tud_control_xfer(rhport, p_request, &audio_samp_freq, 4);
      }
      if (p_request->bRequest == AUDIO20_CS_REQ_RANGE) {
        audio20_control_range_4_n_t(1) range;
        range.wNumSubRanges = 1;
        range.subrange[0].bMin = (int32_t) audio_samp_freq;
        range.subrange[0].bMax = (int32_t) audio_samp_freq;
        range.subrange[0].bRes = 0;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &range, sizeof(range));
      }
      return false;
    }
    if (ctrlSel == AUDIO20_CS_CTRL_CLK_VALID) {
      return tud_control_xfer(rhport, p_request, &audio_clk_valid, 1);
    }
    return false;
  }

  if (entity == 2) { // Feature Unit
    if (ctrlSel == AUDIO20_FU_CTRL_MUTE) {
      return tud_control_xfer(rhport, p_request, &audio_mute[ch], 1);
    }
    if (ctrlSel == AUDIO20_FU_CTRL_VOLUME) {
      if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
        return tud_control_xfer(rhport, p_request, &audio_volume[ch], 2);
      }
      if (p_request->bRequest == AUDIO20_CS_REQ_RANGE) {
        audio20_control_range_2_n_t(1) range;
        range.wNumSubRanges = 1;
        range.subrange[0].bMin = -90;
        range.subrange[0].bMax = 90;
        range.subrange[0].bRes = 1;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &range, sizeof(range));
      }
    }
    return false;
  }

  if (entity == 1) { // Input Terminal: channel cluster (1 ch, no spatial info, no names)
    if (ctrlSel == AUDIO20_TE_CTRL_CONNECTOR) {
      static const uint8_t cluster[3] = {1, 0, 0};
      return tud_control_xfer(rhport, p_request, (void *) cluster, sizeof(cluster));
    }
    return false;
  }

  return false;
}

extern "C" bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
  (void) rhport;
  uint8_t ch      = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t entity  = TU_U16_HIGH(p_request->wIndex);
  if (ch > 1) ch = 1;

  if (entity == 4 && ctrlSel == AUDIO20_CS_CTRL_SAM_FREQ && p_request->bRequest == AUDIO20_CS_REQ_CUR) {
    audio_samp_freq = tu_unaligned_read32(pBuff);
    return true;
  }
  if (entity == 2) {
    if (ctrlSel == AUDIO20_FU_CTRL_MUTE && p_request->bRequest == AUDIO20_CS_REQ_CUR) {
      audio_mute[ch] = pBuff[0];
      return true;
    }
    if (ctrlSel == AUDIO20_FU_CTRL_VOLUME && p_request->bRequest == AUDIO20_CS_REQ_CUR) {
      audio_volume[ch] = (int16_t) tu_unaligned_read16(pBuff);
      return true;
    }
  }
  return false;
}
