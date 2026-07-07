# Shrike Lite — RP2040 MPSSE Emulation Bridge

Hardware interfacing bridge that turns an RP2040-based **Shrike Lite** board into a single-channel, high-speed FTDI FT232H-compatible USB device, speaking the native FTDI MPSSE (Multi-Protocol Synchronous Serial Engine) protocol directly over USB. This lets standard host tooling — OpenOCD's mainline `ftdi` adapter driver, `libftdi`, `pyftdi` — talk to Shrike Lite as if it were genuine FTDI hardware, with no custom host-side driver required.

> **Status: Phase-2 bring-up in progress.** USB enumeration, descriptor-level FTDI identity, and EP0 control-transfer handling are confirmed working. **Bulk data transfer is not yet reliable** — see [issue](https://github.com/ORG/REPO/issues/ISSUE_NUMBER).

---

## Project Brief & Goals

Shrike Lite's RP2040 firmware is a derivative of [phdussud/pico-dirtyJtag](https://github.com/phdussud/pico-dirtyJtag), originally built around DirtyJTAG's own native USB command protocol. Phase-2 replaces that native protocol with an MPSSE opcode interpreter, so the board enumerates and behaves as an FTDI FT232H (VID `0x0403`, PID `0x6014`) instead of a custom vendor device.

**Goal:** a driverless OpenOCD hardware debugging interface — plug in Shrike Lite, point OpenOCD's stock `ftdi` driver at it, and get a working JTAG connection to a target (currently validated against an STM32F4 target board) with no custom OpenOCD build and no custom host driver.

## How It Works

```
Host PC (OpenOCD / pyftdi / libftdi)
        │  USB bulk + control transfers
        ▼
TinyUSB device stack (single vendor-class interface, EP0 control handling)
        │
        ▼
MPSSE opcode parser (cmd.c) — stateful across USB packets
        │
        ▼
RP2040 PIO state machine (pio_jtag.c) — TCK/TDI/TDO/TMS bit-shifting
        │
        ▼
Physical JTAG pins → target device
```

The device broadcasts the standard FTDI VID/PID pair `0403:6014` (FT232H, single channel). No FPGA involved — this is a pure RP2040 PIO + TinyUSB implementation.

## Repository Structure

```
.
├── pico-dirtyJtag-master/    # Firmware source (CMake project, TinyUSB + PIO)
├── dirtyJtag.uf2             # Pre-compiled binary — flash this directly, no build required
├── README.md
└── .gitignore
```

If you want to modify firmware, build from `pico-dirtyJtag-master/` with the Pico SDK. Otherwise, `dirtyJtag.uf2` at the repo root is ready to flash as-is.

## Prerequisites

- **Target hardware:** Shrike Lite (RP2040-based board), an STM32F4 target board for JTAG testing
- **Host environment:** Windows with WSL (Windows Subsystem for Linux)
- **[usbipd-win](https://github.com/dorssel/usbipd-win/releases)** — tested against `5.3.0`, for USB device passthrough into WSL
- **OpenOCD** — see [Known Issues](#known-issues) for which build to use
- Python 3 + `pyftdi` (`pip3 install pyftdi --break-system-packages`) — used for isolated smoke-testing independent of OpenOCD

## Step-by-Step Implementation Guide

### Step A: Flashing the Firmware

1. Hold the **BOOTSEL** button on the RP2040 while plugging it into your PC via USB.
2. It will mount as a mass-storage drive. Drag and drop `dirtyJtag.uf2` (from the repo root) onto it.
3. The board reboots automatically and re-enumerates as the FTDI-compatible device.

### Step B: Binding the USB Device to WSL

The RP2040 now identifies as an FTDI device, so it needs to be passed through to your WSL Linux environment.

Open an **Administrator** Windows Command Prompt:
```powershell
usbipd list
```
Find the Bus ID for `FT232H Single HS USB-UART/FIFO IC` (VID `0403`, PID `6014`), then:
```powershell
usbipd bind --busid <busid>
usbipd attach --wsl --busid <busid>
```

### Step C: Verifying the Interface & Permissions

In your WSL terminal:
```bash
lsusb | grep -i "0403:6014"
```
You should see `Future Technology Devices International, Ltd FT232H Single HS USB-UART/FIFO IC`.

For raw USB access without needing `sudo` every time, add a udev rule:
```bash
sudo tee /etc/udev/rules.d/99-shrike-lite.rules << 'EOF'
SUBSYSTEM=="usb", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6014", MODE="0666"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger
```
Unplug/replug (or re-run `usbipd attach`) after adding the rule for it to take effect. Alternatively, ensure your user is in the `plugdev` group, or fall back to `sudo` for testing.

### Step D: Installing OpenOCD

**Try this first — mainline OpenOCD, no fork needed:**
```bash
sudo apt update && sudo apt install openocd
```
This ships the stock `ftdi` adapter driver, which is what Shrike Lite's MPSSE emulation is designed to work with directly.

**If that doesn't get you a working connection**, fall back to the [jeanthom/openocd-dirtyjtag](https://github.com/jeanthom/openocd-dirtyjtag) fork, which adds a dedicated `dirtyjtag` adapter driver speaking DirtyJTAG's original native protocol (VID `0x1209`, PID `0xC0CA`) instead of MPSSE. This targets the pre-Phase-2 native protocol, not the FTDI emulation this README is primarily about — treat it as a fallback / comparison path, not the primary route.

### Step E: Connecting to the Target (STM32F4)

With mainline OpenOCD and the board enumerated as `0403:6014`:
```bash
sudo openocd -c "adapter driver ftdi" -c "ftdi_vid_pid 0x0403 0x6014" -c "ftdi_channel 0" -c "ftdi_layout_init 0x0008 0x000b" -c "transport select jtag" -c "adapter speed 1000" -f target/stm32f4x.cfg
```

**If this connects and completes target detection: great, you're done — that's the primary supported path.**

**If OpenOCD hangs at `mpsse_flush()`** — this is a known, tracked problem, see [issue #ISSUE_NUMBER](https://github.com/ORG/REPO/issues/ISSUE_NUMBER). While it's open, you have two options:
1. Wait for a firmware fix and re-flash an updated `dirtyJtag.uf2`, or
2. Fall back to `jeanthom/openocd-dirtyjtag`'s `dirtyjtag` driver against the original native protocol, understanding this bypasses the MPSSE/FTDI-emulation work entirely.

## Acknowledgements

- [phdussud/pico-dirtyJtag](https://github.com/phdussud/pico-dirtyJtag) — this firmware is a direct derivative of this project
- [jeanthom/openocd-dirtyjtag](https://github.com/jeanthom/openocd-dirtyjtag) — OpenOCD driver for DirtyJTAG's native protocol, used as a fallback path
- [pyftdi](https://github.com/eblot/pyftdi) — used for isolated MPSSE smoke-testing independent of OpenOCD
- [TinyUSB](https://github.com/hathach/tinyusb) — USB device stack this firmware is built on
