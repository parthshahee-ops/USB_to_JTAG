#include "tusb.h"
#include <string.h>

// Define the absolute USB Device Descriptor properties
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200, // USB 2.0 Specification Compliance
    .bDeviceClass       = 0x00,   // Class defined at Interface level
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    
    .idVendor           = 0x0403, // FTDI Vendor ID
    .idProduct          = 0x6014, // FT232H Product ID (Excellent stability for raw bulk endpoints)
    .bcdDevice          = 0x0900, // Device revision
    
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const * tud_descriptor_device_cb(void) {
    return (uint8_t const *) &desc_device;
}

// Map the Configuration Descriptor Matrix
enum {
    ITF_NUM_VENDOR = 0,
    ITF_NUM_MAX
};

// FIXED: TUD_VENDOR_DESC_LEN is not defined in all TinyUSB/pico-sdk bundles and
// can silently evaluate to 0, producing a 9-byte truncated configuration descriptor.
// The USB host reads the truncated descriptor, fails enumeration, and the device
// never appears on the bus. Use explicit byte counts instead:
//   TUD_CONFIG_DESC_LEN  = 9  bytes  (standard configuration descriptor)
//   Vendor interface     = 23 bytes  (9 interface + 7 OUT endpoint + 7 IN endpoint)
//   Total                = 32 bytes
#define CONFIG_TOTAL_LEN  (9 + 23)

#define EPNUM_VENDOR_OUT  0x01
#define EPNUM_VENDOR_IN   0x81

// FIXED: The last argument to TUD_VENDOR_DESCRIPTOR is epsize (wMaxPacketSize),
// not the software RX buffer size. For Full Speed bulk endpoints the value must
// be exactly 64. Passing CFG_TUD_VENDOR_RX_BUFSIZE happened to equal 64 here,
// but the semantic mismatch can cause descriptor corruption on other SDK versions.
uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_MAX, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 0, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 64)
};

uint8_t const * tud_descriptor_configuration_cb(uint8_t index) {
    (void) index;
    return desc_configuration;
}

// Static array explicitly assigned to flash memory region to prevent pointer crashes
static char const* string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },    // 0: Supported Languages (English)
    "FTDI",                           // 1: Manufacturer String
    "FT232H JTAG Emulation",          // 2: Product Name
    "SHRIKE-LITE-V1"                  // 3: Serial Identification Number
};

static uint16_t _desc_str[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;
    uint8_t chr_count;

    if (index == 0) {
        _desc_str[1] = string_desc_arr[0][0] | (string_desc_arr[0][1] << 8);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) return NULL;
        const char* str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for(uint8_t i=0; i<chr_count; i++) {
            _desc_str[1+i] = str[i];
        }
    }
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2*chr_count + 2));
    return _desc_str;
}