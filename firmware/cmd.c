#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/spi.h> // Added for hardware SPI bridging

#include "tusb.h"
#include "pio_jtag.h"
#include "cmd.h"
#include "DirtyJtagConfig.h" // Pulls in your SPI pinout

// The new USB packet handler for Shrike Lite
void cmd_handle(pio_jtag_inst_t* jtag, uint8_t* rxbuf, uint32_t count, uint8_t* tx_buf) {
  if (count == 0) return;

  uint8_t cmd = rxbuf[0];
  uint16_t bit_len = 0;
  uint16_t byte_len = 0;
  
  // 1. Wake up the FPGA SPI Slave
  gpio_put(PIN_SPI_CS, 0);

  // 2. Route the packet based on the Shrike Lite PDF Specification
  switch(cmd) {
      
      // --- 16-Bit Length Commands ---
      // --- 16-Bit Length Commands ---
        case 0xEE: // TYPE_TDO_EXIT
        case 0xEF: // TYPE_TDI_EXIT
        case 0xF0: // TYPE_TDI
        case 0xF1: // TYPE_TDO
            if (count >= 3) {
            bit_len = (rxbuf[1] << 8) | rxbuf[2];
            byte_len = (bit_len + 7) / 8; // ceil(bits / 8)

            // Calculate the total packet size (CMD + LEN_H + LEN_L + DATA)
            uint8_t total_spi_bytes = 3 + byte_len;

            // 1. PHYSICAL LOOPBACK: Write to MOSI and read from MISO simultaneously
            spi_write_read_blocking(SPI_PORT, rxbuf, tx_buf, total_spi_bytes);

            // 2. Return the captured physical echo back to the PC
            // (Only returning data for commands that OpenOCD/Python expect data from)
            if (cmd == 0xEE || cmd == 0xF1) {
                tud_vendor_write(tx_buf, total_spi_bytes);
                tud_vendor_flush();  
            }
        }
        break;

      // --- 8-Bit Length Command ---
        case 0xF2: // TYPE_TMS
            if (count >= 2) {
                bit_len = rxbuf[1];
                byte_len = (bit_len + 7) / 8;
                spi_write_blocking(SPI_PORT, rxbuf, 2 + byte_len);
            }
        break;

      // --- Frequency Command ---
        case 0xF3: // TYPE_SPEED
            if (count >= 3) {
                spi_write_blocking(SPI_PORT, rxbuf, 3);
            }
        break;

      default:
          // Unrecognized command, drop it
          break;
  }

  // 3. End SPI Transaction
  gpio_put(PIN_SPI_CS, 1);
}