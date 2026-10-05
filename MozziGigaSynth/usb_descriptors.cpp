// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com
// Part of Mozzi-tinyusb. See LICENSE at the repository root.

// usb_descriptors.cpp — UAC2 microphone descriptors for the GIGA build.
//
// Port of ../SineToneTest/usb_descriptors.cpp: 48 kHz, 16-bit,
// mono capture device. Distinct PID (0x5002) so a Pico running 0x5001 stays
// identifiable if both are ever attached to one host.

#include <ArduinoTinyUSB.h>

enum { ITF_NUM_AUDIO_AC = 0, ITF_NUM_AUDIO_AS, ITF_NUM_TOTAL };

// 0x81 = EP1 IN, isochronous. 98 B = 49 samples = one 1 ms full-speed frame at
// 48 kHz (TUD_AUDIO_EP_SIZE(0, 48000, 2, 1)); TinyUSB's clock-deviation
// compensation sends 96 B nominal and 98 B when the device is ahead, so do not
// "fix" the 49th sample.
#define EP_IN_ADDR 0x81
#define EP_IN_SZ   98

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO20_MIC_ONE_CH_DESC_LEN)

static uint8_t const desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 200),
    TUD_AUDIO20_MIC_ONE_CH_DESCRIPTOR(
        ITF_NUM_AUDIO_AC,   // _itfnum
        0x00,               // _stridx
        2,                  // _nBytesPerSample
        16,                 // _nBitsUsedPerSample
        EP_IN_ADDR,
        EP_IN_SZ),
};

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},  // 0: English
    "sensorium",                 // 1: Manufacturer
    "Mozzi GIGA Synth",          // 2: Product
    "0001",                      // 3: Serial
};

static uint16_t _desc_str[32];

static tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,               // UAC2
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x2341,
    .idProduct = 0x5002,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

extern "C" uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *)&desc_device;
}
extern "C" uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return desc_fs_configuration;
}
extern "C" uint16_t const *tud_descriptor_string_cb(uint8_t index,
                                                   uint16_t langid) {
  (void)langid;
  uint8_t chr_count;
  if (index == 0) {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  } else {
    if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
      return NULL;
    const char *str = string_desc_arr[index];
    chr_count = (uint8_t)strlen(str);
    if (chr_count > 31) chr_count = 31;
    for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = str[i];
  }
  _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);
  return _desc_str;
}
