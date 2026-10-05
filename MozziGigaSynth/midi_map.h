// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com
// Part of Mozzi-tinyusb. See LICENSE at the repository root.

#pragma once
// nanoKONTROL2 CC map (CC mode: hold Set Marker + Cycle while plugging in).
// Verified live 2026-09-30 (ccCount 959, CC0/CC16/CC21 streams observed).
#include <Arduino.h>

// Faders: CC 0-7 (ch 1) | Knobs: CC 16-23 (ch 1)
static const uint8_t CC_FADER[8] = {0, 1, 2, 3, 4, 5, 6, 7};
static const uint8_t CC_KNOB[8] = {16, 17, 18, 19, 20, 21, 22, 23};
// Solo / Mute / Rec-arm rows
static const uint8_t CC_SOLO[8] = {32, 33, 34, 35, 36, 37, 38, 39};
static const uint8_t CC_MUTE[8] = {48, 49, 50, 51, 52, 53, 54, 55};
static const uint8_t CC_REC[8] = {64, 65, 66, 67, 68, 69, 70, 71};
// Transport (Korg defaults)
static const uint8_t CC_PLAY = 41;
static const uint8_t CC_STOP = 42;

// Live CC cache: written by tuh_midi_rx_cb, read by updateControl().
volatile uint8_t ccCache[128] = {0};
volatile bool ccSeen[128] = {false};
volatile uint32_t ccCount = 0;
volatile uint8_t lastCC = 0;
volatile uint8_t lastVal = 0;
