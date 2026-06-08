#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "tusb.h"
#include "bsp/board.h"   // Required for board_init() — sets up clocks + USB PLL

// Define Shrike Lite Physical Pin Mapping routed across the interconnect bridge
#define PIN_TCK 2  // Out (Maps to Bit 0 / TXD in the driver)
#define PIN_TDI 3  // Out (Maps to Bit 1 / RXD in the driver)
#define PIN_TMS 4  // Out (Maps to Bit 3 / CTS in the driver)
#define PIN_TDO 5  // In  (Maps to Bit 2 / RTS in the driver)
#define PIN_LED 25

void hardware_jtag_init() {
    gpio_init(PIN_TCK);
    gpio_init(PIN_TDI);
    gpio_init(PIN_TMS);
    gpio_init(PIN_TDO);

    gpio_set_dir(PIN_TCK, GPIO_OUT);
    gpio_set_dir(PIN_TDI, GPIO_OUT);
    gpio_set_dir(PIN_TMS, GPIO_OUT);
    gpio_set_dir(PIN_TDO, GPIO_IN);

    gpio_put(PIN_TCK, 0);
    gpio_put(PIN_TDI, 0);
    gpio_put(PIN_TMS, 0);
}

void synchronous_bitbang_engine() {
    if (tud_vendor_available()) {
        uint8_t incoming_pin_packet;
        tud_vendor_read(&incoming_pin_packet, 1);

        uint8_t tck = (incoming_pin_packet >> 0) & 0x01;
        uint8_t tdi = (incoming_pin_packet >> 1) & 0x01;
        uint8_t tms = (incoming_pin_packet >> 3) & 0x01;

        gpio_put(PIN_TCK, tck);
        gpio_put(PIN_TDI, tdi);
        gpio_put(PIN_TMS, tms);

        uint8_t tx_reply[3];
        tx_reply[0] = 0x01;
        tx_reply[1] = 0x60;
        tx_reply[2] = (gpio_get(PIN_TDO) << 2);

        tud_vendor_write(tx_reply, 3);
        tud_vendor_flush();
    }
}

int main(void) {
    // 1. board_init() MUST be first — it configures the system PLL and the
    //    USB 48 MHz clock. tusb_init() will hard-fault without it.
    //    stdio_init_all() does NOT do this — it only hooks up stdio transports.
    board_init();

    // 2. LED on early — if this lights up we know board_init() succeeded
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 1);

    // 3. JTAG GPIO init (safe now that clocks are up)
    hardware_jtag_init();

    // 4. Start TinyUSB — clocks are guaranteed live at this point
    tusb_init();

    // 5. LED off — confirms tusb_init() returned without faulting
    gpio_put(PIN_LED, 0);

    while (1) {
        tud_task();
        synchronous_bitbang_engine();
    }

    return 0;
}