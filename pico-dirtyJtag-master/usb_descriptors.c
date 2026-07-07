/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2020-2025 Patrick Dussud
 * Copyright (c) 2023 David Williams (davidthings)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * ---------------------------------------------------------------------------
 * Shrike Lite Phase-2 / Option-2 bring-up:
 * Pivoted to single-channel FT232H emulation (VID 0x0403 / PID 0x6014) to
 * eliminate dual-channel descriptor/endpoint-mapping as a variable while
 * debugging the mpsse_flush() hang. Only one vendor-class bulk interface
 * now exists: JTAG/MPSSE. The old Channel B (UART passthrough) interface,
 * endpoints, and string descriptor have been removed entirely.
 *
 * This is intended as an internal bring-up / validation milestone, NOT for
 * external distribution -- see Phase-2 roadmap notes on trademark scope
 * before shipping anything using FTDI's VID/PID outside the lab.
 * ---------------------------------------------------------------------------
 */

#include "dirtyJtagConfig.h"
#include "tusb.h"
#include "get_serial.h"

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00, // Each interface specifies its own
    .bDeviceSubClass    = 0x00, // Each interface specifies its own
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0x0403, // FTDI
    .idProduct          = 0x6014, // FT232H (single channel)
    .bcdDevice          = 0x0900, // confirm against real FT232H dump for your libftdi version

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const * tud_descriptor_device_cb(void)
{
  return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
// Single vendor-class (0xFF/0xFF/0xFF) bulk interface, matching FT232H's
// single-MPSSE-channel layout. No CDC / IAD descriptors, no second
// interface -- Channel B / UART passthrough has been fully removed.
//--------------------------------------------------------------------+

enum
{
  ITF_NUM_JTAG = 0, // JTAG / MPSSE (only interface in this build)
  ITF_NUM_TOTAL
};

// Endpoint addressing follows typical FT232H convention.
// Not strictly required to match FTDI's exact addresses for libftdi to work
// (it queries descriptors), but kept aligned to convention to avoid
// surprises with tooling that hardcodes assumptions.
#define JTAG_IN_EP_NUM   0x81
#define JTAG_OUT_EP_NUM  0x02

// Bulk endpoint size: 64B targets full-speed bring-up first.
// Bump to 512B only after confirming Shrike Lite's PHY negotiates
// high-speed and after re-validating libftdi's read-chunking / the
// 2-byte modem-status prefix framing against that packet size.
#define BULK_EP_SIZE 64

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)

uint8_t const desc_configuration[CONFIG_TOTAL_LEN] =
{
  // Config number, interface count, string index, total length, attribute, power in mA
  // bmAttributes 0x80 = bus-powered, no remote wakeup (matches real FT232H)
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80, 90),

  // =========================================================================
  // JTAG/MPSSE - Strictly mapped to 0xFF/0xFF/0xFF
  // =========================================================================
  // Interface Length (9), Descriptor Type (4), Interface Index (0), Alternate Setting (0),
  // Endpoint Count (2), Class (0xFF), Subclass (0xFF), Protocol (0xFF), String Index (4)
  9, TUSB_DESC_INTERFACE, ITF_NUM_JTAG, 0, 2, 0xFF, 0xFF, 0xFF, 4,
  // Endpoint OUT
  7, TUSB_DESC_ENDPOINT, JTAG_OUT_EP_NUM, TUSB_XFER_BULK, (uint8_t)(BULK_EP_SIZE & 0xFF), (uint8_t)((BULK_EP_SIZE >> 8) & 0xFF), 0,
  // Endpoint IN
  7, TUSB_DESC_ENDPOINT, JTAG_IN_EP_NUM, TUSB_XFER_BULK, (uint8_t)(BULK_EP_SIZE & 0xFF), (uint8_t)((BULK_EP_SIZE >> 8) & 0xFF), 0
};

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
  (void) index; // for multiple configurations
  return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// array of pointer to string descriptors
char const *string_desc_arr[] =
{
    (const char[]){0x09, 0x04},  // 0: supported language is English (0x0409)
    "Shrike Lite Project",       // 1: Manufacturer
    "Shrike Lite",                // 2: Product
    usb_serial,                   // 3: Serial, uses flash unique ID
    "Shrike Lite JTAG",            // 4: JTAG/MPSSE interface
};

static uint16_t _desc_str[32];

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void) langid;

  uint8_t chr_count;

  if ( index == 0)
  {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  }else
  {
    // Convert ASCII string into UTF-16

    if ( !(index < sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) ) return NULL;

    const char* str = string_desc_arr[index];

    // Cap at max char
    chr_count = strlen(str);
    if ( chr_count > 31 ) chr_count = 31;

    for(uint8_t i=0; i<chr_count; i++)
    {
      _desc_str[1+i] = str[i];
    }
  }

  // first byte is length (including header), second byte is string type
  _desc_str[0] = (TUSB_DESC_STRING << 8 ) | (2*chr_count + 2);

  return _desc_str;
}