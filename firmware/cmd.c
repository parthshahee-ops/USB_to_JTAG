#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include <pico/stdlib.h>
#include "pio_jtag.h"
#include "tusb.h"
#include "cmd.h"
#include "dirtyJtagConfig.h"

// Set to 1 to trace every command byte over the USB CDC/UART console.
// Uses printf, which routes to whatever stdio is set up in dirtyJtag.c
// (usb_serial_init / cdc_uart_init). Disable once done debugging -
// it will slow things down and can itself desync USB timing.
#define CMD_TRACE 0

// ---------------------------------------------------------
// DirtyJTAG Host-Side Identifiers (USB from PC)
// Verified against phdussud/pico-dirtyJtag upstream cmd.c
// ---------------------------------------------------------
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
  // CMD_XFER
  NO_READ       = 0x80,
  EXTEND_LENGTH = 0x40,
  // CMD_CLK
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

// Largest bit count CMD_XFER can carry in a single 64-byte USB
// bulk packet (2 header bytes + 62 payload bytes).
#define CMD_XFER_MAX_BITS (62 * 8)

// ---------------------------------------------------------
// CMD_INFO
// ---------------------------------------------------------
static uint32_t cmd_info(uint8_t *buffer)
{
  char info_string[10] = "DJTAG2\n";
  memcpy(buffer, info_string, 10);
  return 10;
}

// ---------------------------------------------------------
// CMD_FREQ - commands[1..2] = big-endian frequency in kHz
// ---------------------------------------------------------
static void cmd_freq(pio_jtag_inst_t* jtag, const uint8_t *commands)
{
  jtag_set_clk_freq(jtag, (commands[1] << 8) | commands[2]);
}

// ---------------------------------------------------------
// CMD_XFER - shift transferred_bits of TDI/TDO through the PIO
// ---------------------------------------------------------
static uint32_t cmd_xfer(pio_jtag_inst_t* jtag, const uint8_t *commands,
                          bool extend_length, bool no_read, uint8_t* tx_buf)
{
  uint16_t transferred_bits = commands[1];

  if (extend_length) {
    transferred_bits += 256;
  }

  // Prevent overrunning the bounds.
  if (transferred_bits > CMD_XFER_MAX_BITS) {
    transferred_bits = CMD_XFER_MAX_BITS;
  }

  if (!no_read) {
    // OpenOCD strictly expects a fixed 32-byte block back for XFER reads.
    memset(tx_buf, 0, 32);
    
    // Perform the actual shift. The PIO state machine writes directly into tx_buf.
    jtag_transfer(jtag, transferred_bits, commands + 2, tx_buf);
    
    return 32; // Always return 32 to satisfy OpenOCD's fixed-size read requirement
  }

  // If no read is requested, just shift the data out.
  jtag_transfer(jtag, transferred_bits, commands + 2, NULL);
  return 0;
}

// ---------------------------------------------------------
// CMD_SETSIG - direct bit-banged pin control, used by the host
// to navigate the TAP state machine between CMD_XFER bursts.
// commands[1] = mask, commands[2] = status
// ---------------------------------------------------------
static void cmd_setsig(pio_jtag_inst_t* jtag, const uint8_t *commands)
{
  uint8_t signal_mask   = commands[1];
  uint8_t signal_status = commands[2];

#if CMD_TRACE
  printf("SETSIG mask=0x%02x status=0x%02x (tms=%d)\n",
         signal_mask, signal_status, !!(signal_mask & SIG_TMS));
#endif

  if (signal_mask & SIG_TCK) {
    jtag_set_clk(jtag, signal_status & SIG_TCK);
  }
  if (signal_mask & SIG_TDI) {
    jtag_set_tdi(jtag, signal_status & SIG_TDI);
  }
  if (signal_mask & SIG_TMS) {
    jtag_set_tms(jtag, signal_status & SIG_TMS);
  }
#if !( BOARD_TYPE == BOARD_QMTECH_RP2040_DAUGHTERBOARD )
  if (signal_mask & SIG_TRST) {
    jtag_set_trst(jtag, signal_status & SIG_TRST);
  }
  if (signal_mask & SIG_SRST) {
    jtag_set_rst(jtag, signal_status & SIG_SRST);
  }
#endif
}

// ---------------------------------------------------------
// CMD_GETSIG - report current TDO level
// ---------------------------------------------------------
static uint32_t cmd_getsig(pio_jtag_inst_t* jtag, uint8_t *buffer)
{
  uint8_t signal_status = 0;
  if (jtag_get_tdo(jtag)) {
    signal_status |= SIG_TDO;
  }
  buffer[0] = signal_status;
  return 1;
}

// ---------------------------------------------------------
// CMD_CLK - clock clk_pulses cycles with TMS/TDI held at fixed
// levels. This is the TAP-navigation primitive.
// commands[1] = signals (SIG_TMS / SIG_TDI bits)
// commands[2] = clk_pulses
// ---------------------------------------------------------
static uint32_t cmd_clk(pio_jtag_inst_t *jtag, const uint8_t *commands,
                         bool readout, uint8_t *buffer)
{
  uint8_t signals    = commands[1];
  uint8_t clk_pulses = commands[2];

#if CMD_TRACE
  printf("CLK signals=0x%02x pulses=%d tms=%d tdi=%d readout=%d\n",
         signals, clk_pulses, !!(signals & SIG_TMS), !!(signals & SIG_TDI), readout);
#endif

  uint8_t readout_val = jtag_strobe(jtag, clk_pulses,
                                     signals & SIG_TMS,
                                     signals & SIG_TDI);

  if (readout) {
    buffer[0] = readout_val;
  }
  return readout ? 1 : 0;
}

static void cmd_setvoltage(const uint8_t *commands)
{
  (void)commands;
}

static void cmd_gotobootloader(void)
{
}

// ---------------------------------------------------------
// Command Handler for Native PIO JTAG
// ---------------------------------------------------------
void cmd_handle(pio_jtag_inst_t* jtag, uint8_t* rxbuf, uint32_t count, uint8_t* tx_buf) {
  if (count == 0) return;

  uint8_t *commands = (uint8_t*)rxbuf;
  uint8_t *output_buffer = tx_buf;

  while ((commands < (rxbuf + count)) && (*commands != CMD_STOP))
  {
    switch ((*commands) & 0x0F) {

    case CMD_INFO:
    {
      uint32_t trbytes = cmd_info(output_buffer);
      output_buffer += trbytes;
      break;
    }

    case CMD_FREQ:
      cmd_freq(jtag, commands);
      commands += 2;
      break;

    case CMD_XFER:
    {
      bool no_read = *commands & NO_READ;
  #if CMD_TRACE
      printf("XFER bits=%d no_read=%d ext=%d\n", commands[1],
             no_read, !!(*commands & EXTEND_LENGTH));
  #endif
      uint32_t trbytes = cmd_xfer(jtag, commands, *commands & EXTEND_LENGTH, no_read, output_buffer);
      
      output_buffer += trbytes; 
      commands += 31; // OpenOCD XFER commands are always 32 bytes total (1 cmd + 31 payload)
      break;
    }

    case CMD_SETSIG:
      cmd_setsig(jtag, commands);
      commands += 2;
      break;

    case CMD_GETSIG:
    {
      uint32_t trbytes = cmd_getsig(jtag, output_buffer);
      output_buffer += trbytes;
      break;
    }

    case CMD_CLK:
    {
      uint32_t trbytes = cmd_clk(jtag, commands, !!(*commands & READOUT), output_buffer);
      output_buffer += trbytes;
      commands += 2;
      break;
    }

    case CMD_SETVOLTAGE:
      cmd_setvoltage(commands);
      commands += 1;
      break;

    case CMD_GOTOBOOTLOADER:
      cmd_gotobootloader();
      break;

    default:
      return; /* Unsupported command, halt */
    }

    commands++;
  }

  /* Send the transfer response back to host */
  if (tx_buf != output_buffer)
  {
    tud_vendor_write(tx_buf, output_buffer - tx_buf);
    tud_vendor_flush();
  }
  return;
}
