/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2020-2025 Patrick Dussud
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
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#include "dirtyJtagConfig.h"

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

// defined by compiler flags for flexibility
#ifndef CFG_TUSB_MCU
  #error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_RHPORT0_MODE     OPT_MODE_DEVICE

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                 OPT_OS_PICO
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//------------- CLASS -------------//
#define CFG_TUD_HID             0

// CDC removed for Phase-2 FT232H bring-up: this is a single vendor-class
// bulk interface speaking MPSSE, not CDC-ACM -- real FT232H does not
// expose CDC at the USB level.
#define CFG_TUD_CDC              0
#define CFG_TUD_MSC             0
#define CFG_TUD_MIDI            0

// Single vendor interface (FT232H, single channel). The earlier
// FT2232H/dual-channel design (Channel A JTAG + Channel B UART) has been
// dropped in favor of this simpler single-interface pivot.
#define CFG_TUD_VENDOR          1

// 128/64 is a reasonable starting point; revisit once JTAG/MPSSE scan
// batching is in place -- larger RX/TX may help throughput.
#define CFG_TUD_VENDOR_RX_BUFSIZE 128
#define CFG_TUD_VENDOR_TX_BUFSIZE 64

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */