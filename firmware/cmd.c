#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/spi.h>

#include "tusb.h"
#include "pio_jtag.h"
#include "cmd.h"
#include "DirtyJtagConfig.h"

void cmd_handle(pio_jtag_inst_t* jtag, uint8_t* rxbuf, uint32_t count, uint8_t* tx_buf)
{
    if (count == 0) return;

    uint8_t cmd = rxbuf[0];

    // Assert CS — begin SPI transaction
    gpio_put(PIN_SPI_CS, 0);
    busy_wait_us(1); 

    switch (cmd) {

        // ---------------------------------------------------------------
        // TYPE_TDO_EXIT (0xEE), TYPE_TDI_EXIT (0xEF), TYPE_TDI (0xF0), TYPE_TDO (0xF1)
        // Format: [CMD] [LEN_H] [LEN_L] [DATA...]
        // ---------------------------------------------------------------
        case 0xEE:
        case 0xEF:
        case 0xF0:
        case 0xF1:
            if (count >= 3) {
                // Parse the 16-bit bit-length and convert to bytes (ceil(LEN / 8))
                uint16_t bit_len = (rxbuf[1] << 8) | rxbuf[2];
                uint32_t byte_len = (bit_len + 7) / 8;

                // SAFETY CLAMP: Prevent buffer overflow if host sends a bad length.
                // Max bulk packet is 64 bytes. Subtract 3 bytes for the header.
                if (byte_len > 61) {
                    byte_len = 61; 
                }

                // Transfer data starting AFTER the 3-byte header (&rxbuf[3])
                spi_write_read_blocking(SPI_PORT, &rxbuf[3], tx_buf, byte_len);

                // For TDO commands, host expects exactly byte_len bytes in reply
                if (cmd == 0xEE || cmd == 0xF1) {
                    tud_vendor_write(tx_buf, byte_len);
                    tud_vendor_flush();
                }
            }
            break;

        // ---------------------------------------------------------------
        // TYPE_TMS (0xF2)
        // Format: [0xF2] [LEN] [DATA...] (LEN is a 1-byte field)
        // ---------------------------------------------------------------
        case 0xF2:
            if (count >= 2) {
                uint8_t bit_len  = rxbuf[1];
                uint8_t byte_len = (bit_len + 7) / 8;
                
                if (byte_len > 62) byte_len = 62;
                
                spi_write_blocking(SPI_PORT, &rxbuf[2], byte_len);
            }
            break;

        // ---------------------------------------------------------------
        // TYPE_SPEED (0xF3)
        // Format: [0xF3] [FREQ_H] [FREQ_L]
        // ---------------------------------------------------------------
        case 0xF3:
            if (count >= 3) {
                // Optional: You can parse this to dynamically change SPI speed,
                // or just pass it to the FPGA if it handles its own clocks.
                spi_write_blocking(SPI_PORT, rxbuf, 3);
            }
            break;

        default:
            break;
    }

    // Deassert CS — end SPI transaction
    gpio_put(PIN_SPI_CS, 1);
}