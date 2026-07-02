# USB-JTAG using RP2040

A modified **DirtyJTAG** firmware for the **RP2040**, designed to provide a reliable, high-speed USB-to-JTAG bridge for debugging and programming ARM-based microcontrollers (such as STM32 devices) using **OpenOCD**.

Unlike the original DirtyJTAG implementation, this firmware focuses on improving JTAG communication stability by correcting packet formatting, TAP state transitions, and USB transfer handling while leveraging the RP2040's **PIO** and **DMA** hardware for efficient JTAG operations.

---

# Features

* Native **USB-to-JTAG** bridge running entirely on the RP2040
* Direct PIO-driven JTAG communication (no external translation hardware)
* High-speed JTAG shifting using RP2040 PIO + DMA
* Compatible with OpenOCD DirtyJTAG driver
* Supports programming and debugging ARM Cortex-M targets

---

# Architecture

```
                 USB
                  │
                  ▼
          ┌─────────────────┐
          │      Host PC    │
          │     OpenOCD     │
          └─────────────────┘
                  │
          DirtyJTAG Protocol
                  │
                  ▼
      ┌─────────────────────────┐
      │        RP2040           │
      │  Modified DirtyJTAG FW  │
      └─────────────────────────┘
                  │
           TCK TMS TDI TDO
                  │
                  ▼
       ┌─────────────────────┐
       │ Target MCU (STM32)  │
       └─────────────────────┘
```

---

# Why This Firmware?

Traditional DirtyJTAG implementations occasionally experience issues such as:

* Improper TAP state transitions
* Variable-length USB packet responses
* OpenOCD assertion failures
* Communication stalls during large JTAG transfers

# Repository Structure

```text
.
├── firmware/
│   ├── CMakeLists.txt
│   ├── dirtyJtag.c
│   ├── cmd.c
│   └── dirtyJtagConfig.h
│   └── flash_4_blink.bin
│
├── software/
│   └── dirtyjtag.cfg
│
└── README.md
```

## Directory Overview

### `firmware/`

Modified RP2040 firmware.

| File                | Description                                                                           |
| ------------------- | ------------------------------------------------------------------------------------- |
| `dirtyJtag.c`       | Main firmware entry point and PIO scheduler                                           |
| `cmd.c`             | USB command parser, protocol handler, TAP navigation, and fixed packet implementation |
| `dirtyJtagConfig.h` | Board-specific pin mappings and configuration                                         |
| `CMakeLists.txt`    | Build configuration                                                                   |
| `flash_4_blink.bin` | OpenOCD interface configuration for the modified firmware                             |

### `software/`

Host-side OpenOCD configuration.

| File            | Description                                               |
| --------------- | --------------------------------------------------------- |
| `dirtyjtag.cfg` | OpenOCD interface configuration for the modified firmware |

---

# Prerequisites

Install the following tools before building:

* Raspberry Pi Pico SDK
* CMake (3.13 or newer)
* Ninja
* ARM GCC Toolchain
* OpenOCD (compiled with DirtyJTAG support)

---

# Installation

## 1. Install the Pico SDK

Install the Pico SDK and configure the environment variable:

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
```

---

## 2. Clone DirtyJTAG

Clone the original DirtyJTAG repository:

```bash
git clone https://github.com/jeanthom/DirtyJTAG.git
cd DirtyJTAG
```

---

## 3. Replace the Source Files

Replace the following files with the versions from this repository:

```
cmd.c
dirtyJtag.c
dirtyJtagConfig.h
```
---

# Building

Create a build directory and compile the firmware:

```bash
mkdir build
cd build

cmake -G "Ninja" -DPICO_SDK_PATH="C:\Program Files\Raspberry Pi\Pico SDK v1.5.1\pico-sdk" ..
"C:\Program Files\Raspberry Pi\Pico SDK v1.5.1\ninja\ninja.exe"
```

After a successful build, the generated UF2 image will be located in the build directory.

---

# Flashing the RP2040

1. Hold the **BOOTSEL** button.
2. Connect the RP2040 to your PC.
3. Release the button once the **RPI-RP2** storage device appears.
4. Copy the generated UF2 file onto the drive.

The board will automatically reboot and enumerate as a USB DirtyJTAG device.

---

# Wiring

Connect the RP2040 JTAG pins to the target MCU.

| RP2040            | Target |
| ----------------- | ------ |
| TCK               | TCK    |
| TMS               | TMS    |
| TDI               | TDI    |
| TDO               | TDO    |
| GND               | GND    |

> Ensure both boards share a common ground.

---

# Programming a Target Device

Use the supplied OpenOCD configuration:

```bash
sudo openocd \
    -f software/dirtyjtag.cfg \
    -c "program flash_4_blink.bin 0x08000000 verify reset exit"
```

---

# Expected Output

A successful programming session should:

* Detect the DirtyJTAG interface
* Select JTAG transport
* Configure a 2000 kHz JTAG clock
* Discover the target TAP(s)
* Detect the Cortex-M core
* Halt the processor
* Erase flash
* Program the firmware
* Verify flash contents
* Reset the target
* Exit without errors

---

# Troubleshooting

### OpenOCD cannot detect the target

* Verify JTAG wiring.
* Check power to the target board.
* Confirm TCK, TMS, TDI, and TDO connections.
* Ensure a common ground between boards.

---

### USB device not detected

* Reflash the RP2040 firmware.
* Check USB cable quality.
* Verify the firmware built successfully.

---

# Credits

This project is based on the **DirtyJTAG** project and extends it with:

* RP2040-specific support
* Improved OpenOCD compatibility
* Reliable TAP state management
* Fixed USB packet handling
* Stable high-speed JTAG communication

Special thanks to the original DirtyJTAG developers [https://github.com/phdussud/pico-dirtyJtag/tree/master] for providing the foundation for this work.

---

# License

This project inherits the licensing terms of the original DirtyJTAG project unless otherwise specified.
