#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include <pico/stdlib.h>
#include "pio_jtag.h"
#include "tusb.h"
#include "cmd.h"
#include "led.h"
#include "dirtyJtagConfig.h"

// Set to 1 to trace every opcode over the USB CDC/UART console.
// Uses printf; disable once done debugging, it will desync USB timing.
#define CMD_TRACE 0

// Channel A vendor interface index (JTAG/MPSSE). Must match ITF_NUM_CHANNEL_A
// in usb_descriptors.c.
#define MPSSE_ITF 0

// ---------------------------------------------------------------------------
// FTDI SIO / MPSSE opcode identifiers
// ---------------------------------------------------------------------------
enum MpsseOpcode {
  // Data shifting - byte-oriented
  OP_SHIFT_OUT_NEG    = 0x11, // clock out on -edge, MSB, bytes, no read
  OP_SHIFT_OUT_POS    = 0x10, // clock out on +edge, MSB, bytes, no read
  OP_SHIFT_IN_POS     = 0x20, // clock in on +edge, MSB, bytes, no write
  OP_SHIFT_IN_NEG     = 0x24, // clock in on -edge, MSB, bytes, no write
  OP_SHIFT_INOUT_A    = 0x30,
  OP_SHIFT_INOUT_B    = 0x31,
  OP_SHIFT_INOUT_C    = 0x34,
  OP_SHIFT_INOUT_D    = 0x35,

  // Data shifting - bit-oriented (same read/write shape as the byte-mode
  // ops above, just with the bit-mode flag 0x02 set and a 1-8 bit count
  // instead of a 2-byte byte count). Needed so host bit-level TDI/TMS
  // framing during odd-length scans doesn't fall into the default case.
  OP_SHIFT_BITS_OUT_POS   = 0x12, // bit-mode version of OP_SHIFT_OUT_POS
  OP_SHIFT_BITS_OUT_NEG   = 0x13, // bit-mode version of OP_SHIFT_OUT_NEG
  OP_SHIFT_BITS_IN_POS    = 0x22, // bit-mode version of OP_SHIFT_IN_POS
  OP_SHIFT_BITS_IN_NEG    = 0x26, // bit-mode version of OP_SHIFT_IN_NEG
  OP_SHIFT_BITS_INOUT_A   = 0x32, // bit-mode version of OP_SHIFT_INOUT_A
  OP_SHIFT_BITS_INOUT_B   = 0x33, // bit-mode version of OP_SHIFT_INOUT_B
  OP_SHIFT_BITS_INOUT_C   = 0x36, // bit-mode version of OP_SHIFT_INOUT_C
  OP_SHIFT_BITS_INOUT_D   = 0x37, // bit-mode version of OP_SHIFT_INOUT_D

  // TMS shifting. Bit 0x20 is the read-enable bit for these opcodes (same
  // meaning as for the byte/bit shift ops above) -- 0x4A/0x4B do NOT read
  // TDO back, only 0x6A/0x6B do. Previously only 0x4B/0x6B were handled,
  // both unconditionally pushing a readback byte -- which silently
  // corrupted packet framing whenever the host issued a write-only 0x4B
  // TMS shift (an extra unrequested byte was being sent back to the host).
  OP_TMS_SHIFT_POS_NOREAD = 0x4A,
  OP_TMS_SHIFT_NEG_NOREAD = 0x4B,
  OP_TMS_SHIFT_POS_READ   = 0x6A,
  OP_TMS_SHIFT_NEG_READ   = 0x6B,

  // GPIO / config
  OP_SET_BITS_LOW     = 0x80,
  OP_SET_BITS_HIGH    = 0x82,
  OP_GET_BITS_LOW     = 0x81,
  OP_GET_BITS_HIGH    = 0x83,
  OP_LOOPBACK_START   = 0x84,
  OP_LOOPBACK_END     = 0x85,
  OP_SET_TCK_DIVISOR  = 0x86,

  OP_SEND_IMMEDIATE   = 0x87
};

// Data-bits-low bit assignments (standard MPSSE GPIO-L mapping)
enum {
  BIT_TCK = 1 << 0,
  BIT_TDI = 1 << 1,
  BIT_TDO = 1 << 2,
  BIT_TMS = 1 << 3
};

// ---------------------------------------------------------------------------
// Parser state machine
// ---------------------------------------------------------------------------
// MPSSE commands can span multiple 64-byte USB bulk packets (data-shift
// payloads can be up to 65536 bytes). We keep state between calls to
// cmd_handle() rather than assuming one packet == one complete command.
//
// There are two distinct ways a command can be split across packets, and
// they need two distinct recovery mechanisms:
//
//   1. A stream-data payload (OP_SHIFT_* byte-mode ops) runs out of packet
//      before stream_remaining hits 0. Handled by ST_STREAM_DATA / the
//      stream_* statics below -- this part was already correct.
//
//   2. A *fixed-size* opcode (OP_SET_TCK_DIVISOR, OP_SET_BITS_LOW/HIGH, the
//      TMS-shift opcodes, the bit-mode shift opcodes) arrives with its
//      opcode byte in one packet but one or more of its 1-2 trailing
//      argument bytes not yet delivered (they're in the *next* bulk OUT
//      packet). This case previously had NO real recovery: every call site
//      did `{ p--; goto need_more; }`, and `need_more` just set
//      `p = end` and broke out of the loop -- the opcode byte and however
//      many trailing bytes *did* arrive were silently discarded, and
//      parser state carried nothing across the call boundary. The next
//      cmd_handle() call started back at a fresh opcode read on whatever
//      byte came next in the new packet, desyncing the whole parse from
//      that point on. This is likely the actual explanation for opcodes
//      "vanishing" whenever a multi-opcode host batch (e.g.
//      jtag_examine_chain()'s TMS-shift + big INOUT shift + TMS-shift
//      sequence) happened to straddle a 64-byte USB packet boundary.
//
//      Fixed below with a small pending-opcode buffer (pending_buf /
//      pending_len / pending_need) that holds whatever partial bytes have
//      arrived so far and resumes cleanly on the next call.
typedef enum {
  ST_IDLE,             // waiting for a fresh opcode byte
  ST_STREAM_DATA       // mid-stream data payload bytes
} parser_state_t;

static parser_state_t state = ST_IDLE;
static uint32_t stream_remaining = 0; // bytes left to consume for current stream op
static bool     stream_write     = false; // does this op consume TDI bytes from host
static bool     stream_read      = false; // does this op produce TDO bytes to host

// Pending-opcode resume buffer: holds a fixed-size opcode (1 opcode byte +
// up to 2 argument bytes = 3 bytes max in this protocol) across a
// cmd_handle() call boundary, when it arrived split across two USB
// packets. pending_len is how many bytes are currently buffered;
// pending_need is the total bytes required (opcode + all its args) before
// the opcode can be dispatched.
static uint8_t pending_buf[3];
static uint8_t pending_len  = 0;
static uint8_t pending_need = 0;

// Output accumulation for the current cmd_handle() call. The 2-byte FTDI
// modem-status header must precede the first byte of every Bulk IN
// transaction, so it's written once per cmd_handle() invocation, not once
// per opcode.
static uint8_t  out_buf[64]; //changed from 256 to 64
static uint32_t out_len = 0;

// Maximum number of consecutive zero-progress iterations we'll tolerate
// before giving up on a write. At ~1us/iteration in the MULTICORE
// tight_loop_contents() path this is a generous but bounded timeout --
// previously there was no cap at all, so a stalled endpoint that still
// reported tud_mounted()==true (host present but not draining) would spin
// Core 0 forever. Bails out and drops the remainder of the packet rather
// than hanging.
#define SAFE_USB_WRITE_MAX_STALL_ITERS 5000000u

static inline void safe_usb_write(const uint8_t *buf, uint32_t len) {
    uint32_t written = 0;
    uint32_t stall_iters = 0;
    while (written < len) {
        uint32_t w = tud_vendor_n_write(MPSSE_ITF, buf + written, len - written);
        written += w;

        if (!tud_mounted()) break;

        if (written < len) {
            if (w == 0) {
                if (++stall_iters >= SAFE_USB_WRITE_MAX_STALL_ITERS) {
                    // Host isn't draining the endpoint -- give up rather than
                    // hang. Remaining bytes are dropped; caller has no way to
                    // be told, but a dropped packet beats a wedged firmware.
                    break;
                }
            } else {
                stall_iters = 0;
            }

            tud_vendor_n_flush(MPSSE_ITF);

            // CRITICAL FIX: Only call tud_task() if we are running in single-core mode.
            // If MULTICORE is defined, Core 0 is already running tud_task() in the background.
#ifndef MULTICORE
            tud_task(); 
#else
            // In multicore mode, just yield a few cycles to let Core 0 drain the FIFO
            tight_loop_contents(); 
#endif
        }
    }
    tud_vendor_n_flush(MPSSE_ITF);
}

static inline void out_flush(void)
{
  if (out_len > 2) {    
    // Use the safe blocking write to guarantee delivery
    safe_usb_write(out_buf, out_len);
    
    // Re-arm the mandatory 2-byte FTDI modem-status header
    out_buf[0] = 0x01;
    out_buf[1] = 0x60;
    out_len = 2;
  }
}

static inline void out_push(uint8_t byte)
{
  if (out_len >= sizeof(out_buf)) {
    out_flush();
  }
  out_buf[out_len++] = byte;
}

// Writes the mandatory FTDI 2-byte modem status header (idle/healthy line
// state: 0x01, 0x60) at the start of the response accumulation buffer for
// this call. Must be the first thing in out_buf before any real data.
static inline void out_begin_packet(void)
{
  out_buf[0] = 0x01;
  out_buf[1] = 0x60;
  out_len = 2;
}

// ---------------------------------------------------------------------------
// FTDI divisor -> jtag_set_clk_freq(kHz) conversion
// Standard FTDI formula (non-high-speed-clock mode):
//   TCK_freq = 12MHz / ((1 + divisor) * 2)
// ---------------------------------------------------------------------------
static void apply_tck_divisor(pio_jtag_inst_t *jtag, uint16_t divisor)
{
  uint32_t freq_hz  = 12000000UL / ((1 + (uint32_t)divisor) * 2);
  uint32_t freq_khz = freq_hz / 1000;
  if (freq_khz == 0) freq_khz = 1;
#if CMD_TRACE
  printf("SET_TCK_DIVISOR div=%u -> %lu kHz\n", divisor, (unsigned long)freq_khz);
#endif
  jtag_set_clk_freq(jtag, freq_khz);
}

// ---------------------------------------------------------------------------
// GPIO bit-bang: OP_SET_BITS_LOW / OP_SET_BITS_HIGH
// value/direction semantics: only pins configured as outputs (direction=1)
// actually drive; TDO (bit 2) is always an input on real hardware regardless
// of direction byte, so we don't attempt to drive it.
// ---------------------------------------------------------------------------
static void apply_set_bits_low(pio_jtag_inst_t *jtag, uint8_t value, uint8_t direction)
{
#if CMD_TRACE
  printf("SET_BITS_LOW value=0x%02x dir=0x%02x\n", value, direction);
#endif
  if (direction & BIT_TCK) jtag_set_clk(jtag, !!(value & BIT_TCK));
  if (direction & BIT_TDI) jtag_set_tdi(jtag, !!(value & BIT_TDI));
  if (direction & BIT_TMS) jtag_set_tms(jtag, !!(value & BIT_TMS));
}

static void apply_set_bits_high(uint8_t value, uint8_t direction)
{
  // High-byte GPIO bank (ADBUS7 / upper bits on real FT2232H) isn't wired
  // to any JTAG-meaningful signal in this design. Stub safely: accept and
  // discard so the host's init sequence doesn't stall waiting on us.
  (void)value; (void)direction;
#if CMD_TRACE
  printf("SET_BITS_HIGH (stub) value=0x%02x dir=0x%02x\n", value, direction);
#endif
}

static uint8_t sample_bits_low(pio_jtag_inst_t *jtag)
{
  uint8_t v = 0;
  if (jtag_get_tdo(jtag)) v |= BIT_TDO;
  // TCK/TDI/TMS readback isn't tracked as separate input state in
  // pio_jtag.c (they're drive-only from this side); report 0 for those bits.
  return v;
}

// ---------------------------------------------------------------------------
// Data shift dispatch helpers - map opcode to (write?, read?) shape.
// Edge selection (falling vs rising, e.g. 0x10 vs 0x11) can't be
// distinguished by the current PIO program (jtag.pio fixes the tck/tdi/tdo
// phase relationship in hardware) -- known limitation, flagged rather than
// silently ignored. Same-direction opcodes are treated identically.
// ---------------------------------------------------------------------------
static void begin_stream_op(uint8_t opcode, uint32_t nbytes)
{
  stream_remaining  = nbytes;
  stream_write      = false;
  stream_read       = false;

  switch (opcode) {
    case OP_SHIFT_OUT_NEG:
    case OP_SHIFT_OUT_POS:
      stream_write = true;
      break;
    case OP_SHIFT_IN_POS:
    case OP_SHIFT_IN_NEG:
      stream_read = true;
      break;
    case OP_SHIFT_INOUT_A:
    case OP_SHIFT_INOUT_B:
    case OP_SHIFT_INOUT_C:
    case OP_SHIFT_INOUT_D:
      stream_write = true;
      stream_read  = true;
      break;
    default:
      break;
  }
}

// Consumes as many stream bytes as are available in [*pp, end), advancing
// *pp. Returns when either the input is exhausted or the stream op
// completes. Safe to call repeatedly across multiple cmd_handle() calls.
static void pump_stream(pio_jtag_inst_t *jtag, const uint8_t **pp, const uint8_t *end)
{
  // NOTE: read-only shift ops (OP_SHIFT_IN_POS/NEG) carry no payload bytes
  // from the host at all -- the host just sends the opcode+length and then
  // waits for TDO data back. Gating this loop on "*pp < end" (as before)
  // meant read-only ops would never run a single iteration once the input
  // was exhausted, leaving stream_remaining > 0 and the parser wedged in
  // ST_STREAM_DATA forever -- exactly the mpsse_flush() hang seen during
  // jtag_examine_chain()'s IDCODE readback. Only require unread input bytes
  // when this op actually consumes them (stream_write); read-only ops just
  // keep clocking until stream_remaining hits 0.
  while (stream_remaining > 0 && (!stream_write || *pp < end)) {
    uint8_t in_byte  = stream_write ? **pp : 0x00;
    uint8_t out_byte = 0;

    if (stream_write) (*pp)++;

    if (stream_read) {
      jtag_transfer(jtag, 8, &in_byte, &out_byte);
      out_push(out_byte);
    } else if (stream_write) {
      jtag_transfer(jtag, 8, &in_byte, NULL);

      // CRITICAL FIX: Prevent USB timeout on long write-only streams!
      // Write-only ops (e.g. bitstream push) never call out_push()/
      // out_flush(), so without this the USB stack never gets serviced
      // for the whole duration of a large stream and the host times out.
      // Service the TinyUSB background stack every 64 bytes.
      if ((stream_remaining & 0x3F) == 0) {
#ifndef MULTICORE
        tud_task();
#else
        tight_loop_contents();
#endif
      }
    }

    stream_remaining--;
  }
}

// ---------------------------------------------------------------------------
// TMS shift: OP_TMS_SHIFT_{POS,NEG}_{NOREAD,READ}
// Followed by: Length byte (bits, 0-7 meaning 1-8 bits), then 1 data byte
// where bit0..bit(n-1) are shifted into TMS, TDI held at fixed level from
// bit 7 of the same data byte per FTDI spec (kept fixed for whole burst).
// Read-back is only pushed when the opcode's read-enable bit (0x20) is
// set (0x6A/0x6B) -- 0x4A/0x4B are write-only and must NOT get a byte
// pushed, or every later response in the same packet shifts by one byte.
// ---------------------------------------------------------------------------
static void apply_tms_shift(pio_jtag_inst_t *jtag, uint8_t opcode, uint8_t length_byte, uint8_t data_byte)
{
  uint8_t nbits = (length_byte & 0x07) + 1;
  bool tdi_level = !!(data_byte & 0x80);
  bool want_read = !!(opcode & 0x20);
  uint8_t readout = 0;

#if CMD_TRACE
  printf("TMS_SHIFT opcode=0x%02x nbits=%d data=0x%02x tdi=%d read=%d\n",
         opcode, nbits, data_byte, tdi_level, want_read);
#endif

  for (uint8_t i = 0; i < nbits; i++) {
    bool tms_bit = !!(data_byte & (1 << i));
    uint8_t shift_pos = 8 - nbits + i; 
    readout |= ((jtag_strobe(jtag, 1, tms_bit, tdi_level) & 1) << shift_pos);
  }
  if (want_read) out_push(readout);
}

// ---------------------------------------------------------------------------
// Bit-mode data shift: OP_SHIFT_BITS_* (opcode bit 0x02 set)
// Followed by: Length byte (bits, 0-7 meaning 1-8 bits), then 1 data byte
// IF the opcode is write-enabled (bit 0x10 set). Same write/read shape as
// the byte-mode ops, just for a partial (<=8 bit) final chunk of a scan.
// Edge selection (falling vs rising) has the same known limitation noted
// on the byte-mode ops: not distinguishable by the current PIO program.
// ---------------------------------------------------------------------------
static void apply_bit_shift(pio_jtag_inst_t *jtag, uint8_t opcode, uint8_t length_byte, uint8_t data_byte)
{
  uint8_t nbits    = (length_byte & 0x07) + 1;
  bool    do_write = !!(opcode & 0x10);
  bool    do_read  = !!(opcode & 0x20);
  uint8_t in_byte  = do_write ? data_byte : 0x00;
  uint8_t out_byte = 0;

#if CMD_TRACE
  printf("BIT_SHIFT opcode=0x%02x nbits=%d data=0x%02x write=%d read=%d\n",
         opcode, nbits, data_byte, do_write, do_read);
#endif

  if (do_read) {
    jtag_transfer(jtag, nbits, &in_byte, &out_byte);
    out_push(out_byte);
  } else if (do_write) {
    jtag_transfer(jtag, nbits, &in_byte, NULL);
  }
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const * request) {
  if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR) {
    if (stage == CONTROL_STAGE_SETUP) {
      
      // === IN REQUESTS (Host asking RP2040 for data) ===
      if (request->bmRequestType_bit.direction == TUSB_DIR_IN) {
        
        // 1. Handle EEPROM Read (0x90) - Structurally valid FT232H Image
        if (request->bRequest == 0x90) {
            // Structurally valid FT232H Image with mathematically correct CRC checksum (0x18C7)
            static const uint16_t ftdi_eeprom_image[64] = {
                0x0000, 0x0403, 0x6014, 0x0900, 0x0000, 0x0000, 0x0000, 0x0000,
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x18C7
            };
            uint16_t word_offset = request->wIndex;
            
            if (word_offset < 64) {
                // Calculate exactly how many bytes are left in our array from this offset
                // (64 total words - current word offset) * 2 bytes per word
                uint16_t max_bytes_left = (64 - word_offset) * 2;
                
                // Only send the requested length, OR the max bytes left (whichever is smaller)
                uint16_t safe_len = request->wLength;
                if (safe_len > max_bytes_left) {
                    safe_len = max_bytes_left;
                }
                
                tud_control_xfer(rhport, request, (void*)&ftdi_eeprom_image[word_offset], safe_len);
            } else {
                tud_control_xfer(rhport, request, NULL, 0); 
            }
          }
        // 2. Handle Latency Timer (0x0A)
        else if (request->bRequest == 0x0A) {
            static uint8_t latency_timer[1] = {16}; 
            uint16_t len = request->wLength > 1 ? 1 : request->wLength;
            tud_control_xfer(rhport, request, latency_timer, len);
        }
        // 3. Handle Modem Status (0x05)
        else if (request->bRequest == 0x05) {
            static uint8_t modem_status[2] = {0x01, 0x60}; 
            uint16_t len = request->wLength > 2 ? 2 : request->wLength;
            tud_control_xfer(rhport, request, modem_status, len);
        }
        // 4. Generic fallback for other IN requests (prevents STALL)
        else {
            static uint8_t dummy_response[2] = {0, 0};
            uint16_t len = request->wLength > 2 ? 2 : request->wLength;
            tud_control_xfer(rhport, request, dummy_response, len);
        }
      }
      // === OUT REQUESTS (Host configuring RP2040) ===
      else {
        // Correctly ACKs FT_SetBitMode, Reset, etc.
        tud_control_xfer(rhport, request, NULL, 0); 
      }
    }
    return true; 
  }
  return false;
}

// Dispatches one fully-assembled fixed-size opcode. `args` points at the
// bytes following the opcode byte (0, 1, or 2 of them depending on
// opcode); `opcode` is the opcode byte itself. Only used for opcodes
// handled via the pending-buffer resume path so the dispatch logic isn't
// duplicated between the "all bytes already in this packet" fast path and
// the "resumed from a previous partial packet" path below.
static void dispatch_fixed_opcode(pio_jtag_inst_t *jtag, uint8_t opcode, const uint8_t *args)
{
  switch (opcode) {
    case OP_SET_TCK_DIVISOR:
      apply_tck_divisor(jtag, (uint16_t)args[0] | ((uint16_t)args[1] << 8));
      break;

    case OP_SET_BITS_LOW:
      apply_set_bits_low(jtag, args[0], args[1]);
      break;

    case OP_SET_BITS_HIGH:
      apply_set_bits_high(args[0], args[1]);
      break;

    case OP_TMS_SHIFT_POS_NOREAD:
    case OP_TMS_SHIFT_NEG_NOREAD:
    case OP_TMS_SHIFT_POS_READ:
    case OP_TMS_SHIFT_NEG_READ:
      apply_tms_shift(jtag, opcode, args[0], args[1]);
      break;

    case OP_SHIFT_BITS_OUT_POS:
    case OP_SHIFT_BITS_OUT_NEG:
    case OP_SHIFT_BITS_IN_POS:
    case OP_SHIFT_BITS_IN_NEG:
    case OP_SHIFT_BITS_INOUT_A:
    case OP_SHIFT_BITS_INOUT_B:
    case OP_SHIFT_BITS_INOUT_C:
    case OP_SHIFT_BITS_INOUT_D:
    {
      bool do_write = !!(opcode & 0x10);
      uint8_t length_byte = args[0];
      uint8_t data_byte   = do_write ? args[1] : 0;
      apply_bit_shift(jtag, opcode, length_byte, data_byte);
      break;
    }

    default:
      // Stream-header opcodes (OP_SHIFT_OUT_*/OP_SHIFT_IN_*/OP_SHIFT_INOUT_*)
      // are handled specially by the caller since they transition into
      // ST_STREAM_DATA rather than completing immediately -- see
      // cmd_handle() below. This function is never called for those.
      break;
  }
}

// ---------------------------------------------------------------------------
// Main entry point. Called once per received USB packet (up to 64 bytes)
// on Channel A. State persists across calls via the static parser_state_t
// and the pending_buf/stream_* statics.
// ---------------------------------------------------------------------------
void cmd_handle(pio_jtag_inst_t* jtag, uint8_t* rxbuf, uint32_t count)
{
  led_tx(1); // diagnostic: confirms cmd_handle() is being entered (GPIO4 on Shrike Lite)

  if (count == 0) return;

  const uint8_t *p   = rxbuf;
  const uint8_t *end = rxbuf + count;

  out_begin_packet();

  while (p < end) {

    // Mid-stream data payload continuation from a previous call
    if (state == ST_STREAM_DATA) {
      pump_stream(jtag, &p, end);
      if (stream_remaining == 0) state = ST_IDLE;
      continue;
    }

    // Resuming a fixed-size opcode whose opcode/arg bytes were split
    // across the previous packet boundary: top up pending_buf from this
    // packet before deciding whether we can dispatch yet.
    if (pending_len > 0) {
      while (pending_len < pending_need && p < end) {
        pending_buf[pending_len++] = *p++;
      }
      if (pending_len < pending_need) {
        // Still not enough even with this packet's worth of data -- stay
        // pending and wait for the next call.
        break;
      }
      uint8_t opcode = pending_buf[0];
      switch (opcode) {
        case OP_SHIFT_OUT_NEG:
        case OP_SHIFT_OUT_POS:
        case OP_SHIFT_IN_POS:
        case OP_SHIFT_IN_NEG:
        case OP_SHIFT_INOUT_A:
        case OP_SHIFT_INOUT_B:
        case OP_SHIFT_INOUT_C:
        case OP_SHIFT_INOUT_D:
        {
          // Stream-header opcode resumed from a split packet: this starts
          // a stream rather than completing a one-shot dispatch, so it
          // can't go through dispatch_fixed_opcode() (whose default case
          // is a no-op for these). len_lo/len_hi are pending_buf[1]/[2].
          uint8_t len_lo = pending_buf[1], len_hi = pending_buf[2];
          uint32_t nbytes = ((uint32_t)len_lo | ((uint32_t)len_hi << 8)) + 1;
          begin_stream_op(opcode, nbytes);
          state = ST_STREAM_DATA;
          pending_len = 0;
          pump_stream(jtag, &p, end);
          if (stream_remaining == 0) state = ST_IDLE;
          continue;
        }
        default:
          dispatch_fixed_opcode(jtag, opcode, &pending_buf[1]);
          break;
      }
      pending_len = 0;
      continue;
    }

    uint8_t opcode = *p++;

    switch (opcode) {

      case OP_LOOPBACK_START:
      case OP_LOOPBACK_END:
        // Internal echo/sync stubs - no line-level side effect required.
        break;

      case OP_SET_TCK_DIVISOR:
      case OP_SET_BITS_LOW:
      case OP_SET_BITS_HIGH:
      case OP_TMS_SHIFT_POS_NOREAD:
      case OP_TMS_SHIFT_NEG_NOREAD:
      case OP_TMS_SHIFT_POS_READ:
      case OP_TMS_SHIFT_NEG_READ:
        if (end - p < 2) {
          // Not enough bytes left in this packet for this opcode's 2 args.
          // Buffer what we have (opcode + any partial args) and resume on
          // the next cmd_handle() call instead of silently dropping it --
          // this is the fix for opcodes vanishing whenever a multi-opcode
          // host batch straddled a 64-byte USB packet boundary.
          pending_buf[0] = opcode;
          uint8_t avail = (uint8_t)(end - p);
          for (uint8_t i = 0; i < avail; i++) pending_buf[1+i] = p[i];
          pending_len  = 1 + avail;
          pending_need = 3;
          p = end;
          break;
        }
        dispatch_fixed_opcode(jtag, opcode, p);
        p += 2;
        break;

      case OP_GET_BITS_LOW:
        out_push(sample_bits_low(jtag));
        break;

      case OP_GET_BITS_HIGH:
        // No high-bank GPIO wired; report 0.
        out_push(0x00);
        break;

      case OP_SHIFT_BITS_OUT_POS:
      case OP_SHIFT_BITS_OUT_NEG:
      case OP_SHIFT_BITS_IN_POS:
      case OP_SHIFT_BITS_IN_NEG:
      case OP_SHIFT_BITS_INOUT_A:
      case OP_SHIFT_BITS_INOUT_B:
      case OP_SHIFT_BITS_INOUT_C:
      case OP_SHIFT_BITS_INOUT_D:
        {
          // Read-only bit-shift opcodes (0x22/0x26) send no data byte at
          // all -- only byte-mode reads and any write-enabled op do.
          bool do_write = !!(opcode & 0x10);
          uint32_t need = do_write ? 2 : 1;
          if ((uint32_t)(end - p) < need) {
            pending_buf[0] = opcode;
            uint8_t avail = (uint8_t)(end - p);
            for (uint8_t i = 0; i < avail; i++) pending_buf[1+i] = p[i];
            pending_len  = 1 + avail;
            pending_need = (uint8_t)(1 + need);
            p = end;
            break;
          }
          uint8_t length_byte = p[0];
          uint8_t data_byte   = do_write ? p[1] : 0;
          p += need;
          apply_bit_shift(jtag, opcode, length_byte, data_byte);
        }
        break;

      case OP_SHIFT_OUT_NEG:
      case OP_SHIFT_OUT_POS:
      case OP_SHIFT_IN_POS:
      case OP_SHIFT_IN_NEG:
      case OP_SHIFT_INOUT_A:
      case OP_SHIFT_INOUT_B:
      case OP_SHIFT_INOUT_C:
      case OP_SHIFT_INOUT_D:
        if (end - p < 2) {
          pending_buf[0] = opcode;
          uint8_t avail = (uint8_t)(end - p);
          for (uint8_t i = 0; i < avail; i++) pending_buf[1+i] = p[i];
          pending_len  = 1 + avail;
          pending_need = 3;
          p = end;
          break;
        }
        {
          uint8_t len_lo = p[0], len_hi = p[1];
          p += 2;
          uint32_t nbytes = ((uint32_t)len_lo | ((uint32_t)len_hi << 8)) + 1;
          begin_stream_op(opcode, nbytes);
          state = ST_STREAM_DATA;
          pump_stream(jtag, &p, end);
          if (stream_remaining == 0) state = ST_IDLE;
        }
        break;

      case OP_SEND_IMMEDIATE:
        if (out_len == 2) {
            // Use the safe blocking write here as well
            safe_usb_write(out_buf, out_len);
        } else {
            out_flush();
        }
        break;

    default:
      // Real FT232H responds to unrecognized opcodes with 0xFA + the bad
      // opcode byte -- hosts (including Efinity) rely on this for MPSSE
      // command-sync during connect.
      out_push(0xFA);
      out_push(opcode);
#if CMD_TRACE
      printf("Unknown MPSSE opcode 0x%02x, sent 0xFA sync\n", opcode);
#endif
      break;

    } // end switch
  } // end while (CRITICAL: this brace must be here!)

  out_flush(); // Flush only ONCE after the entire packet is parsed
  led_tx(0); // diagnostic: confirms this flush path executes

} // end cmd_handle
