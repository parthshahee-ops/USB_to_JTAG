/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020-2025 Patrick Dussud
 * Copyright (c) 2021 jeanthom
 * Copyright (c) 2023 David Williams (davidthings)
 * Copyright (c) 2023 Chandler Klüser
 * Copyright (c) 2024 DangerousPrototypes
 * Copyright (c) 2024 DESKTOP-M9CCUTI\ian
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
 */

/*
 * RP2040 DirtyJTAG Firmware - Standard PIO JTAG Mode (STM32 Target)
 *
 * Phase-2 / Option-2 pivot: single-channel FT232H emulation. All
 * Channel B / UART-passthrough logic has been removed. There is now
 * exactly one vendor interface (index 0), so tud_vendor_available()/
 * tud_vendor_read()/tud_vendor_write() (non-indexed) are used directly --
 * no more risk of an index mismatch between the availability check and
 * the read call, which was the actual cause of the mpsse_flush() hang
 * (the old code checked tud_vendor_n_available(1) but read from
 * tud_vendor_n_read(0, ...), so Channel A data was never picked up.)
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/pio.h"
#include "pico/multicore.h"
#include "pio_jtag.h"
#include "led.h"
#include "bsp/board.h"
#include "tusb.h"
#include "cmd.h"
#include "get_serial.h"
#include "hardware/gpio.h"

#include "dirtyJtagConfig.h"

// #define MULTICORE

// ---------------------------------------------------------------------------
// JTAG/PIO peripheral descriptor 
// ---------------------------------------------------------------------------
pio_jtag_inst_t jtag = {
    .pio = pio0,
    .sm  = 0
};

void init_pins(void)
{
    bi_decl(bi_4pins_with_names(PIN_TCK, "TCK", PIN_TDI, "TDI", PIN_TDO, "TDO", PIN_TMS, "TMS"));
#if !( BOARD_TYPE == BOARD_QMTECH_RP2040_DAUGHTERBOARD )
    bi_decl(bi_2pins_with_names(PIN_RST, "RST", PIN_TRST, "TRST"));
#endif
}

void djtag_init(void)
{
    init_pins();
#if !( BOARD_TYPE == BOARD_QMTECH_RP2040_DAUGHTERBOARD )
    init_jtag(&jtag, 1000, PIN_TCK, PIN_TDI, PIN_TDO, PIN_TMS, PIN_RST, PIN_TRST);
#else
    init_jtag(&jtag, 1000, PIN_TCK, PIN_TDI, PIN_TDO, PIN_TMS, 255, 255);
#endif
}

// ---------------------------------------------------------------------------
// Double-buffered command queue (unicore implementation)
// ---------------------------------------------------------------------------
typedef uint8_t cmd_buffer[64];

static uint wr_buffer_number = 0;
static uint rd_buffer_number = 0;

typedef struct {
    volatile uint8_t count;
    volatile uint8_t busy;
    cmd_buffer        buffer;
} buffer_info;

#define N_BUFFERS 4
static buffer_info buffer_infos[N_BUFFERS];

// tx_buf is bounded at 64 bytes (USB full-speed bulk max).
static cmd_buffer tx_buf;

// ---------------------------------------------------------------------------
void jtag_main_task(void)
{
#ifdef MULTICORE
    if (multicore_fifo_rvalid()) {
        uint rx_num       = multicore_fifo_pop_blocking();
        buffer_info *bi   = &buffer_infos[rx_num];
        bi->busy          = false;
    }
#endif

    if (!buffer_infos[wr_buffer_number].busy) {
        tud_task();

        // Single vendor interface now (CFG_TUD_VENDOR=1): JTAG/MPSSE
        // traffic -> cmd_handle() pipeline. Non-indexed tud_vendor_*
        // calls are unambiguous again since there's only one interface.
        if (tud_vendor_available()) {
            led_rx(1);
            uint bnum  = wr_buffer_number;
            uint count = tud_vendor_read(buffer_infos[bnum].buffer, 64);
            if (count != 0) {
                buffer_infos[bnum].count = count;
                buffer_infos[bnum].busy  = true;
                wr_buffer_number         = (wr_buffer_number + 1) % N_BUFFERS;
#ifdef MULTICORE
                multicore_fifo_push_blocking(bnum);
#endif
            }
            led_rx(0);
        }
    }
}

void jtag_task(void)
{
#ifndef MULTICORE
    jtag_main_task();
#endif
}

#ifdef MULTICORE
void core1_entry(void)
{
    djtag_init();
    while (1) {
        uint rx_num     = multicore_fifo_pop_blocking();
        buffer_info *bi = &buffer_infos[rx_num];
        assert(bi->busy);
        cmd_handle(&jtag, bi->buffer, bi->count, tx_buf);
        multicore_fifo_push_blocking(rx_num);
    }
}
#endif

void fetch_command(void)
{
#ifndef MULTICORE
    if (buffer_infos[rd_buffer_number].busy) {
        cmd_handle(&jtag,
                   buffer_infos[rd_buffer_number].buffer,
                   buffer_infos[rd_buffer_number].count,
                   tx_buf);
        buffer_infos[rd_buffer_number].busy = false;
        rd_buffer_number = (rd_buffer_number + 1) % N_BUFFERS;
    }
#endif
}

// ---------------------------------------------------------------------------
// FTDI SIO control requests (EP0). libftdi/pyftdi/OpenOCD send these during
// device open (SIO_RESET) and periodically (SIO_SET_BITMODE when entering
// MPSSE mode, SIO_SET_LATENCY_TIMER, etc). Previously this callback
// unconditionally STALLed every vendor control request, which broke
// mpsse_open() before any bulk data could ever flow (LIBUSB_ERROR_PIPE).
//
// Minimum viable behavior: ACK (return true) all known FTDI SIO bRequest
// values so host-side opens succeed. Most are no-ops functionally right now;
// SIO_SET_BITMODE is the one that matters going forward (that's the request
// that tells the device "enter MPSSE mode" - currently accepted but ignored
// since the single interface only ever speaks MPSSE in this build).
// ---------------------------------------------------------------------------
#define FTDI_SIO_RESET              0x00
#define FTDI_SIO_SET_MODEM_CTRL     0x01
#define FTDI_SIO_SET_FLOW_CTRL      0x02
#define FTDI_SIO_SET_BAUD_RATE      0x03
#define FTDI_SIO_SET_DATA           0x04
#define FTDI_SIO_SET_LATENCY_TIMER  0x09
#define FTDI_SIO_GET_LATENCY_TIMER  0x0A
#define FTDI_SIO_SET_BITMODE        0x0B
#define FTDI_SIO_READ_EEPROM        0x90

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                 tusb_control_request_t const *request)
{
    (void)rhport;

    if (stage != CONTROL_STAGE_SETUP) return true;

    switch (request->bRequest) {
        case FTDI_SIO_RESET:
        case FTDI_SIO_SET_MODEM_CTRL:
        case FTDI_SIO_SET_FLOW_CTRL:
        case FTDI_SIO_SET_BAUD_RATE:
        case FTDI_SIO_SET_DATA:
        case FTDI_SIO_SET_LATENCY_TIMER:
        case FTDI_SIO_SET_BITMODE:
            // No hardware action needed for most of these yet; ACK so the
            // host's open/init sequence doesn't stall. Revisit
            // SET_BITMODE if runtime MPSSE mode switching is ever needed.
            return tud_control_status(rhport, request);

        case FTDI_SIO_GET_LATENCY_TIMER:
        {
            // Host expects 1 byte back: current latency timer value (ms).
            // Report a fixed placeholder value; not yet tracked per-device.
            uint8_t latency = 16;
            return tud_control_xfer(rhport, request, &latency, 1);
        }

        case FTDI_SIO_READ_EEPROM:
            // No emulated EEPROM contents; ACK with zeroed data so chip-type
            // probing doesn't hang, rather than STALLing and failing open.
        {
            uint8_t eeprom_stub[2] = {0x00, 0x00};
            return tud_control_xfer(rhport, request, eeprom_stub, sizeof(eeprom_stub));
        }

        default:
            // Unknown vendor request: STALL as before, don't silently
            // pretend to support requests we haven't reasoned about.
            return false;
    }
}

// ===========================================================================
// main — hardware bring-up sequence (Reverted for Standard JTAG)
// ===========================================================================
int main(void)
{
    board_init();
    // --- ADD THESE THREE LINES TO INITIALIZE THE LED ---
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 0); // Start with it turned off
    led_init(LED_INVERTED, PIN_LED_TX, PIN_LED_RX, PIN_LED_ERROR);

    // =========================================================================
    // HAND OVER TO USB AND JTAG PIO STACK
    // No FPGA bitstream deployment or SPI initialization required for STM32.
    // =========================================================================
    tusb_init();
    usb_serial_init();

    // CRITICAL FIX: We restore djtag_init() to map the physical pins
    // back to the RP2040's native PIO JTAG state machine.
    djtag_init();

    // UART passthrough (old Channel B) has been removed as part of the
    // single-channel FT232H pivot. If UART bring-up is needed again later,
    // it will need its own dedicated interface/endpoint plan rather than
    // being multiplexed onto this vendor interface.

#ifdef MULTICORE
    multicore_launch_core1(core1_entry);
#endif

    // =========================================================================
    // MAIN LOOP
    // =========================================================================
    while (1) {
        jtag_main_task();
        fetch_command(); 
    }
}