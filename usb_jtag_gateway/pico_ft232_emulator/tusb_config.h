#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#define CFG_TUSB_MCU             OPT_MCU_RP2040
#define CFG_TUD_ENABLED          1

#define CFG_TUD_MAX_SPEED        OPT_MODE_FULL_SPEED

// Allocate a Vendor-Specific Interface instead of standard CDC/MSC/HID
#define CFG_TUD_VENDOR           1
#define CFG_TUD_VENDOR_RX_BUFSIZE 64
#define CFG_TUD_VENDOR_TX_BUFSIZE 64

#endif