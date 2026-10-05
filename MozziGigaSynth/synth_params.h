// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com
// Part of Mozzi-tinyusb. See LICENSE at the repository root.

#pragma once
// Shared synth parameters: written in updateControl(), read in updateAudio().
#include <Arduino.h>

struct SynthParams {
  float oscFreq = 110.0f;      // Hz
  float detune = 1.5f;         // Hz offset for osc2
  uint8_t oscMix = 128;        // 0 = saw only, 255 = square only
  uint8_t cutoff = 128;        // 0-255 filter cutoff
  uint8_t resonance = 40;      // 0-255
  float lfoRateHz = 5.0f;
  uint8_t lfoDepth = 40;       // filter mod depth
  uint8_t vibDepth = 0;        // vibrato depth
  uint8_t drive = 0;           // extra gain 0-127
  uint8_t volume = 200;        // master 0-255
  uint16_t attackMs = 20;
  uint16_t decayMs = 200;
  uint16_t sustainMs = 800;
  uint16_t releaseMs = 300;
  int8_t octaveShift = 0;      // -1/0/+1 from Solo buttons
  bool lfoEnable = true;
  volatile bool gate = true;   // drone on by default; Play/Stop override
};

extern SynthParams params;
