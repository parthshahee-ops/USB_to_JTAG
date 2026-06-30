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

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/pio.h"
#include "pico/multicore.h"
#include "pio_jtag.h"
#include "cdc_uart.h"
#include "led.h"
#include "bsp/board.h"
#include "tusb.h"
#include "cmd.h"
#include "get_serial.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "bitstream.h"

#include "dirtyJtagConfig.h"

// #define MULTICORE

// ---------------------------------------------------------------------------
// JTAG/SPI peripheral descriptor (used by cmd_handle for any JTAG fall-through)
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
// The cmd_handle fix ensures it is never overrun.
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
        // Read the next USB packet immediately after tud_task() so two
        // back-to-back Bulk-OUT packets are never merged (DirtyJTAG protocol
        // requirement).
        tud_task();

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
        } else {
#if ( CDC_UART_INTF_COUNT > 0 )
            cdc_uart_task();
#endif
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

// TinyUSB vendor control transfer callback (boilerplate — library does not
// auto-handle setup requests)
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                 tusb_control_request_t const *request)
{
    (void)rhport; (void)request;
    if (stage != CONTROL_STAGE_SETUP) return true;
    return false;
}

// ===========================================================================
// main — hardware bring-up sequence
//
// BUG FIX SUMMARY (vs. previous version):
//
//  1. TARGET RESET ASSERTED FIRST (critical — was missing entirely)
//     GPIO 14 (PIN_TARGET_RST, active-low) is driven LOW before power rails
//     come up.  Without this, the line floats and the FPGA's internal reset
//     domain is undefined during bitstream deployment, which can corrupt the
//     configuration state machine even before user logic starts.
//
//  2. POWER-RAIL STABILISATION DELAY INCREASED (100 ms → 500 ms)
//     The MicroPython reference script waits 500 ms after asserting power.
//     100 ms was marginal for regulators with soft-start curves; 500 ms is
//     safe and matches the proven working script.
//
//  3. MISO PULL-UP ADDED
//     gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI) only routes the pin mux;
//     it does NOT configure a pull resistor.  MicroPython's SPI constructor
//     silently adds a pull-up.  Without it, any brief high-Z transient on
//     MISO (e.g. the FPGA output-enable glitch during CS assertion) resolves
//     to the floating rail voltage — typically below the input threshold —
//     and the RP2040 reads 0x00.  gpio_pull_up() matches the MicroPython
//     implicit behaviour.
//
//  4. RESET PULSE DELIVERED AFTER BITSTREAM (the missing low→high edge)
//     The previous code set GPIO 14 HIGH immediately with no prior LOW phase,
//     so the FPGA's synchronous reset FSM never received its triggering edge
//     and remained stuck in the reset state permanently.  The correct sequence
//     is: assert LOW (Step 0) → deploy bitstream → release HIGH (Step 4).
//
//  5. POST-RESET DELAY INCREASED (50 ms → 1 000 ms)
//     Matches MicroPython reference.  Custom Verilog needs time for any
//     internal oscillator to stabilise, PLLs to lock, and reset-domain
//     crossing synchronisers to clear their pipelines.
//
//  6. REDUNDANT gpio_init(PIN_SPI_CS) AFTER BITSTREAM REMOVED
//     Calling gpio_init() a second time after the bitstream write resets the
//     pin direction and output state, momentarily glitching CS back to input
//     mode.  The CS state is already correct after the initial setup block;
//     the second call was unnecessary and potentially disruptive.
// ===========================================================================

int main(void)
{
    board_init();

    // =========================================================================
    // STEP 0 — ASSERT TARGET RESET BEFORE EVERYTHING ELSE
    // Drive PIN_TARGET_RST LOW immediately so the reset line is never floating.
    // This pin stays LOW through power-up and bitstream deployment.
    // =========================================================================
    gpio_init(PIN_TARGET_RST);
    gpio_set_dir(PIN_TARGET_RST, GPIO_OUT);
    gpio_put(PIN_TARGET_RST, 0); // Active-low: hold target in reset

    // =========================================================================
    // STEP 1 — POWER UP TARGET HARDWARE
    // =========================================================================
    gpio_init(PIN_PWR); gpio_set_dir(PIN_PWR, GPIO_OUT); gpio_put(PIN_PWR, 1);
    gpio_init(PIN_EN);  gpio_set_dir(PIN_EN,  GPIO_OUT); gpio_put(PIN_EN,  1);

    // BUG FIX: was 100 ms — increased to 500 ms to match the MicroPython
    // reference and give all power rails time to fully stabilise before
    // clocking SPI data into the target.
    sleep_ms(500);

    led_init(LED_INVERTED, PIN_LED_TX, PIN_LED_RX, PIN_LED_ERROR);
    led_tx(false); // Solid = power stable

    // =========================================================================
    // STEP 2 — CONFIGURE SPI PERIPHERAL
    // =========================================================================
    spi_init(SPI_PORT, 1000 * 1000);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(PIN_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);

    // BUG FIX: gpio_set_function() only muxes the pin — it does NOT add a
    // pull resistor.  MicroPython's SPI() constructor silently adds a pull-up
    // on MISO.  Any high-Z transient (output-enable glitch during CS edge)
    // without a pull-up causes the RP2040 to read 0x00.
    gpio_pull_up(PIN_SPI_MISO);

    // CS is driven manually (not by the hardware SPI engine) so we get full
    // control over the exact CS timing relative to data.
    gpio_init(PIN_SPI_CS);
    gpio_set_dir(PIN_SPI_CS, GPIO_OUT);
    gpio_put(PIN_SPI_CS, 1); // Deassert CS (idle high)
    sleep_ms(10);

    // =========================================================================
    // STEP 3 — DEPLOY BITSTREAM (target remains in reset during this phase)
    // =========================================================================
    led_tx(true); // Blinking = bitstream deploy in progress

    gpio_put(PIN_SPI_CS, 0); // Assert CS — start configuration frame
    spi_write_blocking(SPI_PORT, fpga_bitstream, fpga_bitstream_len);
    gpio_put(PIN_SPI_CS, 1); // Deassert CS — close configuration frame

    // Send 64 extra SCK pulses (8 bytes × 8 bits) with CS deasserted.
    // Many iCE40 / ECP5 / Gowin devices require post-configuration clocks
    // to latch the bitstream and transition out of config mode.
    uint8_t dummy_clocks[8] = {0};
    spi_write_blocking(SPI_PORT, dummy_clocks, sizeof(dummy_clocks));

    // BUG FIX: the previous code called gpio_init(PIN_SPI_CS) AGAIN here,
    // which reset the pin to input mode and momentarily glitched CS.  Removed.

    sleep_ms(20); // Let configuration latch settle

    // =========================================================================
    // STEP 4 — RELEASE RESET (deliver the missing LOW → HIGH rising edge)
    //
    // BUG FIX: the previous code went straight to gpio_put(14, 1) without ever
    // driving the line LOW first (it was left floating from boot).  The custom
    // Verilog reset FSM requires a valid rising edge to exit reset state.
    // PIN_TARGET_RST was driven LOW in Step 0, so this single gpio_put()
    // delivers the correct 0 → 1 edge.
    // =========================================================================
    gpio_put(PIN_TARGET_RST, 1); // Rising edge: kick FPGA FSM out of reset

    // BUG FIX: was 50 ms — increased to 1 000 ms to match the MicroPython
    // reference.  Custom Verilog needs time for oscillators, PLLs, and
    // reset-domain crossing synchronisers to fully settle.
    sleep_ms(1000);

    led_tx(false); // Solid = boot sequence complete

    // =========================================================================
    // STEP 5 — HAND OVER TO USB STACK
    // Safe to initialise TinyUSB now that the target hardware is fully running.
    // =========================================================================
    tusb_init();
    usb_serial_init();
    //djtag_init();

#if ( CDC_UART_INTF_COUNT > 0 )
    cdc_uart_init(0, PIN_UART0, PIN_UART0_RX, PIN_UART0_TX);
#endif
#if ( CDC_UART_INTF_COUNT > 1 )
    cdc_uart_init(1, PIN_UART1, PIN_UART1_RX, PIN_UART1_TX);
#endif

#ifdef MULTICORE
    multicore_launch_core1(core1_entry);
#endif

    // =========================================================================
    // MAIN LOOP
    // =========================================================================
    while (1) {
        jtag_main_task();
        fetch_command(); // unicore command dispatch
    }
}
