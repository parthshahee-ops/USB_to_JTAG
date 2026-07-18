# Shrike Lite - RP2040 MPSSE Emulation Bridge

Hardware interfacing bridge that turns an RP2040-based **Shrike Lite** board into a single-channel, high-speed FTDI FT232H-compatible USB device, speaking the native FTDI MPSSE (Multi-Protocol Synchronous Serial Engine) protocol directly over USB. The original goal was to let standard host tooling - OpenOCD's mainline `ftdi` adapter driver, `libftdi`, `pyftdi` - talk to Shrike Lite as if it were genuine FTDI hardware, with no custom host-side driver required.

> **Status: Phase-3 bring-up in progress.** Development has pivoted away from OpenOCD. USB enumeration and descriptor-level FTDI identity are confirmed working against Efinity's FPGA programming software, but protocol-level communication beyond enumeration is not yet working - Efinity currently reports an operation timeout. See [Known Issues](#known-issues).

---

## Project Brief & Goals

Shrike Lite's RP2040 firmware is a derivative of [phdussud/pico-dirtyJtag](https://github.com/phdussud/pico-dirtyJtag), originally built around DirtyJTAG's own native USB command protocol. Phase-2 replaced that native protocol with an MPSSE opcode interpreter, so the board enumerates and behaves as an FTDI FT232H (VID `0x0403`, PID `0x6014`) instead of a custom vendor device.

Phase-2 development against OpenOCD's stock `ftdi` driver hit an unresolved `mpsse_flush()` deadlock during bulk transfers (see [Known Issues](#known-issues)). Given time constraints, **development pivoted to Phase-3: bypassing OpenOCD entirely and integrating directly with Efinity's FPGA programming software**, so Shrike Lite can act as a native FTDI-compatible programmer for Efinity-based workflows rather than an OpenOCD JTAG adapter.

**Current goal:** get Shrike Lite recognized and driven directly by Efinity as an FT232H-class MPSSE programmer - enumeration and FTDI identity already work; the remaining effort is on the communication layer.

## How It Works

```
Host PC (Efinity / pyftdi / libftdi)
        │  USB bulk + control transfers
        ▼
TinyUSB device stack (single vendor-class interface, EP0 control handling)
        │
        ▼
MPSSE opcode parser (cmd.c) - stateful across USB packets
        │
        ▼
RP2040 PIO state machine (pio_jtag.c) - TCK/TDI/TDO/TMS bit-shifting
        │
        ▼
Physical JTAG pins → Trion120 FPGA target
```

The device broadcasts the standard FTDI VID/PID pair `0403:6014` (FT232H, single channel). No FPGA involved in the RP2040-side USB/MPSSE stack - this is a pure RP2040 PIO + TinyUSB implementation, driving an on-board Trion120 FPGA over JTAG.

## Repository Structure

```
.
├── pico-dirtyJtag-master/    # Firmware source (CMake project, TinyUSB + PIO)
├── pico_mpsse.c              # Reference MPSSE implementation, adapted from MiSTle-Dev/PICO-MPSSE
├── usb_descriptors.c         # TinyUSB device/config/string descriptors - FTDI VID/PID identity lives here
├── dirtyJtag.uf2             # Pre-compiled binary - flash this directly, no build required
├── README.md
└── .gitignore
```

If you want to modify firmware, build from `pico-dirtyJtag-master/` with the Pico SDK. Otherwise, `dirtyJtag.uf2` at the repo root is ready to flash as-is.

## Reference Implementation: PICO-MPSSE

While debugging the current firmware config, we also referred to [MiSTle-Dev/PICO-MPSSE](https://github.com/MiSTle-Dev/PICO-MPSSE) - an open-source Raspberry Pi Pico MPSSE-compatible JTAG programmer for DIY FPGA projects - and rewrote `pico_mpsse.c` (its core MPSSE/USB implementation) to match Shrike Lite's setup. Although the end goal is still our own FT232H emulation, this port was used as a way to sanity-check firmware/pin configuration against a known-working reference. This surfaced a wiring/config mismatch, documented below.

## Prerequisites

- **Target hardware:** Shrike Lite (RP2040 + Trion120 FPGA board)
- **Host environment:** Windows with WSL (Windows Subsystem for Linux)
- **[usbipd-win](https://github.com/dorssel/usbipd-win/releases)** - tested against `5.3.0`, for USB device passthrough into WSL
- **Efinity** - Efinix's FPGA programming/toolchain software (primary target for Phase-3 integration)
- Python 3 + `pyftdi` (`pip3 install pyftdi --break-system-packages`) - used for isolated smoke-testing independent of Efinity/OpenOCD

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

### Step D: Connecting via Efinity

With the board enumerated as `0403:6014`, open Efinity's programmer tool and select the board as the USB target (shown as `(local) Shrike Lite` with FT232H board profile). Efinity currently detects and identifies the device correctly, but the programming/JTAG session fails - see [Known Issues](#known-issues).

## Changing the USB VID/PID

Shrike Lite currently identifies as FTDI's FT232H (VID `0x0403`, PID `0x6014`) so that stock FTDI tooling recognizes it without a custom driver. This VID/PID pair is defined in a single place: `desc_device` in `usb_descriptors.c`.

To change it, edit the `idVendor` / `idProduct` fields:

```c
tusb_desc_device_t const desc_device =
{
    ...
    .idVendor           = 0x0403, // FTDI
    .idProduct          = 0x6014, // FT232H (single channel)
    ...
};
```

Notes:

- `bcdDevice` (`0x0900`) is the FTDI-reported device release number; if you change VID/PID away from FTDI's, this field is no longer meaningful and can be set to whatever your own descriptor scheme uses.
- The three string descriptors (`iManufacturer`, `iProduct`, `iSerialNumber`) are defined separately in `string_desc_arr[]` in the same file (currently `"Shrike Lite Project"` / `"Shrike Lite"` / `usb_serial`) - update these too if you're moving away from an FTDI-compatible identity, since host tooling and udev rules often match on these alongside VID/PID.
- After changing VID/PID, remember to update any host-side references that key off `0403:6014` - the `usbipd`/udev steps in this README (Steps B and C), and any OpenOCD/pyftdi/Efinity config that hardcodes the FTDI IDs.
- Rebuild and reflash (`Step A`) for the change to take effect; the board will re-enumerate under the new identity.

## Known Issues

### Efinity: Operation Timeout (Phase-3, current blocker)

Efinity correctly selects the FT232H board profile and reports the expected USB target info (`Bus 001 Device 005: ID 0403:6014`), but the connection then fails with:

```
UsbError: [Errno 10060] Operation timed out
```

immediately followed by Efinity falling back to SPI Active mode since JTAG could not be established. Enumeration and descriptor-level FTDI identity are confirmed working; the timeout indicates the MPSSE bulk-transfer/protocol layer is not completing whatever exchange Efinity expects post-enumeration. This is still under investigation, cross-referencing Wireshark/usbmon captures of Efinity's traffic against Shrike Lite's MPSSE implementation.

### PICO-MPSSE reference port: TMS line stuck / no JTAG TAP state transitions

While rewriting `pico_mpsse.c` (adapted from [MiSTle-Dev/PICO-MPSSE](https://github.com/MiSTle-Dev/PICO-MPSSE)) to check firmware/pin configuration, driving the Trion120 from the RP2040 showed TCK toggling continuously on the logic analyzer while TDI/TDO/TMS/TRST stayed flat - no TAP state transitions at all.

**Cause:** `config.h`'s pin definitions (`JTAG1`/`JTAG2`) don't match the actual RP2040↔Trion120 wiring:

- Real wiring: `TCK=GP14`, `TDI=GP13`, `TDO=GP16`, `TMS=GP10`
- `config.h` (JTAG1 & JTAG2) points at different, unrelated GPIOs

As a result, the PIO drives/reads pins that aren't connected to the FPGA. TMS ends up undriven, and per the Trion120 datasheet an undriven TMS is pulled up to logic 1, which holds the TAP permanently in `Test-Logic-Reset` - consistent with the capture.

This isn't a simple `config.h` fix, either: `jtag.pio` requires `pin_tms == pin_tdi + 1` for its interleaved shift instruction, but the real wiring (`TDI=GP13`, `TMS=GP10`) isn't adjacent, so remapping the pin numbers alone won't satisfy the PIO program's assumptions.

### mpsse_flush() hang (Phase-2, OpenOCD - superseded)

Earlier, while targeting OpenOCD's stock `ftdi` driver, the connection would hang indefinitely during DR scan interrogation, with OpenOCD logging repeated `mpsse_flush(): Haven't made progress` warnings. Root cause was not conclusively identified (opcode coverage gaps were found and fixed but did not resolve the hang, suggesting bulk-transfer chunking or timing rather than pure opcode dispatch). Given time constraints, this was not fully root-caused before development pivoted to direct Efinity integration (Phase-3, above). Full details are in the project's internship report.

## Acknowledgements

- [phdussud/pico-dirtyJtag](https://github.com/phdussud/pico-dirtyJtag) - this firmware is a direct derivative of this project
- [MiSTle-Dev/PICO-MPSSE](https://github.com/MiSTle-Dev/PICO-MPSSE) - reference MPSSE/JTAG-programmer implementation, used to sanity-check firmware/pin configuration; `pico_mpsse.c` was adapted from this project
- [jeanthom/openocd-dirtyjtag](https://github.com/jeanthom/openocd-dirtyjtag) - OpenOCD driver for DirtyJTAG's native protocol, used during Phase-1
- [pyftdi](https://github.com/eblot/pyftdi) - used for isolated MPSSE smoke-testing independent of Efinity/OpenOCD
- [TinyUSB](https://github.com/hathach/tinyusb) - USB device stack this firmware is built on
