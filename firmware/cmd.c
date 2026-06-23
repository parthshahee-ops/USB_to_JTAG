#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/spi.h>

#include "jtag.pio.h"
#include "tusb.h"
#include "pio_jtag.h"
#include "cmd.h"

#ifndef SPI_PORT
#define SPI_PORT spi0
#endif

#define SPI_BUF_SIZE 64u

enum CommandIdentifier {
  CMD_STOP           = 0x00,
  CMD_INFO           = 0x01,
  CMD_FREQ           = 0x02,
  CMD_XFER           = 0x03,
  CMD_SETSIG         = 0x04,
  CMD_GETSIG         = 0x05,
  CMD_CLK            = 0x06,
  CMD_SETVOLTAGE     = 0x07,
  CMD_GOTOBOOTLOADER = 0x08
};

enum CommandModifier {
  NO_READ       = 0x80,
  EXTEND_LENGTH = 0x40,
  READOUT       = 0x80
};

enum SignalIdentifier {
  SIG_TCK  = 1 << 1,
  SIG_TDI  = 1 << 2,
  SIG_TDO  = 1 << 3,
  SIG_TMS  = 1 << 4,
  SIG_TRST = 1 << 5,
  SIG_SRST = 1 << 6
};

static inline void cs_assert(void)
{
    gpio_put(PIN_SPI_CS, 0);
    busy_wait_us(1);
}

static inline void cs_deassert(void)
{
    busy_wait_us(1);
    gpio_put(PIN_SPI_CS, 1);
}

// Custom non-blocking hardware FIFO controller
static inline void spi_transfer_fifo(const uint8_t *tx_buf, uint8_t *rx_buf, uint32_t len) 
{
    uint32_t tx_remain = len;
    uint32_t rx_remain = len;

    while (tx_remain > 0 || rx_remain > 0) {
        while (tx_remain > 0 && spi_is_writable(SPI_PORT)) {
            spi_get_hw(SPI_PORT)->dr = (uint32_t)*tx_buf++;
            tx_remain--;
        }
        while (rx_remain > 0 && spi_is_readable(SPI_PORT)) {
            uint8_t rx_byte = (uint8_t)spi_get_hw(SPI_PORT)->dr;
            if (rx_buf) {
                *rx_buf++ = rx_byte;
            }
            rx_remain--;
        }
    }
}

void cmd_handle(pio_jtag_inst_t* jtag, uint8_t* rxbuf, uint32_t count, uint8_t* tx_buf) {
  (void)jtag;
  
  if (count == 0) return;

  uint8_t *commands = (uint8_t*)rxbuf;
  uint8_t *output_buffer = tx_buf;
  
  uint8_t spi_tx[SPI_BUF_SIZE];
  uint8_t spi_rx[SPI_BUF_SIZE];

  // March through the batched buffer using the exact stock structural loop
  while ((commands < (rxbuf + count)) && (*commands != CMD_STOP))
  {
    uint8_t current_cmd = *commands;
    
    switch (current_cmd & 0x0F) {
    
    case CMD_INFO:
    {
      char info_string[10] = "DJTAG2\n";
      memcpy(output_buffer, info_string, 10);
      output_buffer += 10;
      break;
    }
    
    case CMD_FREQ:
      spi_tx[0] = 0xF3;
      spi_tx[1] = commands[1];
      spi_tx[2] = commands[2];

      cs_assert();
      spi_transfer_fifo(spi_tx, NULL, 3);
      cs_deassert();

      commands += 2; // Advance past the 2 argument bytes
      break;

    case CMD_XFER:
    {
      bool no_read = current_cmd & NO_READ;
      uint16_t transferred_bits = commands[1];
      
      if (current_cmd & EXTEND_LENGTH) {
        transferred_bits += 256;
      }
      
      uint16_t byte_len = (transferred_bits + 7) / 8;

      spi_tx[0] = 0xF0;
      spi_tx[1] = (transferred_bits >> 8) & 0xFF;
      spi_tx[2] = transferred_bits & 0xFF;
      memcpy(&spi_tx[3], commands + 2, byte_len);

      cs_assert();
      spi_transfer_fifo(spi_tx, spi_rx, 3 + byte_len);
      cs_deassert();

      if (!no_read) {
        memset(output_buffer, 0, 32);
        memcpy(output_buffer, &spi_rx[3], byte_len); 
        output_buffer += 32;                          
      }
      commands += 31; 
      break;
    }
    
    case CMD_SETSIG:
      commands += 2; // Advance past the 2 argument bytes
      break;

    case CMD_GETSIG:
      output_buffer[0] = 0x00;
      output_buffer += 1;
      break;
      
    case CMD_CLK:
    {
      bool readout = current_cmd & READOUT;
      uint8_t signals = commands[1];
      uint8_t clk_pulses = commands[2];
      
      uint8_t byte_len = (clk_pulses + 7) / 8;
      uint8_t tms_fill = (signals & SIG_TMS) ? 0xFF : 0x00;

      spi_tx[0] = 0xF2;
      spi_tx[1] = clk_pulses;
      memset(&spi_tx[2], tms_fill, byte_len);

      cs_assert();
      spi_transfer_fifo(spi_tx, spi_rx, 2 + byte_len);
      cs_deassert();

      if (readout) {
        output_buffer[0] = spi_rx[2];
        output_buffer += 1;
      }
      
      commands += 2; // Advance past the 2 argument bytes
      break;
    }
    
    case CMD_SETVOLTAGE:
      commands += 1;
      break;

    case CMD_GOTOBOOTLOADER:
      break;
      
    default:
      return; 
    }

    commands++; // Golden loop increment: steps past the opcode byte safely
  }

  /* Flush the completed response block back over USB */
  if (tx_buf != output_buffer)
  {
    tud_vendor_write(tx_buf, output_buffer - tx_buf);
    tud_vendor_flush();
  }
  return;
}
