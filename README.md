# USB-JTAG 

This repository contains a modified firmware implementation (adapted from DirtyJTAG) tailored for the RP2040, along with the accompanying hardware description language and software interface. The system embeds a custom hardware configuration bitstream directly into the C firmware to configure an attached target device over internal SPI traces autonomously, bypassing standard power-sequencing conflicts.

## Architecture Overview

Standard JTAG/SPI bridge implementations often default to pulling power pins LOW upon initialization, which unintentionally wipes the volatile memory of the target hardware during the MCU handoff. 

This custom firmware overrides that safety behavior by:
1. **Locking Power HIGH:** Forcing the voltage regulators (`PIN_PWR` and `PIN_EN`) to stay ON during the boot sequence by mapping the default firmware to dummy pins.
2. **Autonomous Configuration:** Flashing the target directly using an embedded C-header array (`bitstream.h`).
3. **Hardware SPI Handoff:** Sending the mandatory wake-up clock pulses and safely releasing the SPI Chip Select (CS) pin from Software I/O mode back to the RP2040's Hardware SPI engine before initiating data routing.

---

## Repository Structure

```text
.
├── firmware/                       # Modified RP2040 C Firmware
│   ├── CMakeLists.txt              # Build configuration for Pico SDK
│   ├── dirtyJtag.c                 # Custom boot sequence, power lock, and CS handoff
│   ├── cmd.c                       # Custom high-speed protocol translation, 32-byte stride alignment, and SPI routing
│   ├── dirtyJtagConfig.h           # Remapped power pins (28/29)
│   └── bitstream.h                 # Embedded target logic array
├── hardware_code/                  # Hardware Description Logic
│   ├── top.v                       # Top module to verify communication
│   ├── spi_target.v                # Verified SPI protocol core
├── software/                       # Host-Side Execution
│   ├── script_test.py              # Python payload delivery and SPI diagnostic script
│   └── dirtyjtag.cfg               # OpenOCD configuration script for JTAG operations
├── output.txt                      # Sample OpenOCD debug execution log
└── README.md
```

---

## Prerequisites
* **Raspberry Pi Pico SDK** * **CMake** and **Ninja** build tools
* **Python 3** (with `pyusb` installed for the test script)

## Setup & Installation

### 1. Install the Pico SDK
Ensure the Raspberry Pi Pico SDK is installed on your system and your `PICO_SDK_PATH` environment variable is correctly configured.

### 2. Download DirtyJTAG
Clone the original DirtyJTAG repository to your local workspace:
```bash
git clone [https://github.com/jeanthom/DirtyJTAG.git](https://github.com/jeanthom/DirtyJTAG.git)
cd DirtyJTAG
```

### 3. Replace the Core Source Files
To enable the custom boot sequence and SPI loopback payload, replace the default DirtyJTAG files with the modified versions provided in this repository:

- `dirtyJtag.c`: Contains the custom hardware boot, power lock, bitstream deployment sequence, and CS pin handover.

- `cmd.c`: Contains the custom 0xee SPI transfer logic for data routing.

- `dirtyJtagConfig.h`: Remaps DirtyJTAG's default power control to dummy pins (28/29) to prevent the target from losing power during the handoff.

- `bitstream.h`: The generated C-header file containing the embedded target logic array, allowing the RP2040 to flash the hardware autonomously without external intervention.

- `CMakeLists.txt`: The modified build configuration file tailored for the Raspberry Pi Pico SDK, ensuring the custom firmware, headers, and specific board definitions are correctly linked during compilation.

### 4. Embed the Target Bitstream
Before compiling, you must convert your compiled logic (FPGA_bitstream_MCU.bin) into a C header file so the RP2040 can read it natively:

1. Place your compiled .bin file in the same directory as the convert.py script.

2. Run the conversion script:
```text
Bash
python convert.py
```

3. Ensure the generated `bitstream.h` is located in the same directory as your modified `dirtyJtag.c` file.

---

## Building the Firmware
Compile the modified firmware using CMake and Ninja from within the pico-dirtyJtag-master directory:

```text
Bash
mkdir build
cd build
cmake -G Ninja ..
ninja
```

---

## Interpreting the Output Log (```output.txt```)
The '''output.txt''' file included in this repository contains a sample execution trace of OpenOCD running with Level 3 Debugging (```-d3```). This log is highly detailed and tracks every bulk USB transfer between the host PC and the RP2040 bridge.

If you are modifying the protocol or troubleshooting a connection, here is how to read and interpret the key sections of the log:

### 1. The Handshake Phase
Look for lines containing ```dirtyjtag_reset(0,0)``` and ```DR scan interrogation for IDCODE/BYPASS```.

What it means: The host PC successfully found the RP2040 over USB, claimed the interface, and sent the initial ``` CMD_INFO```packets. If the log crashes before this point, your USB passthrough (e.g., ```usbipd```) is likely disconnected.

### 2. The ```syncbb_scan``` Hex Dumps
When OpenOCD executes a data transfer, you will see a block like this:

```text
>>> OPENOCD SENDING TO MCU (32 Bytes Flat Payload) >>>
03 F0 FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
```
- What it means: This shows the exact raw bytes OpenOCD is packaging and sending down the wire.
  - Byte 03 is the CMD_XFER opcode.
  - Byte F0 is the requested bit length (240 bits).
  - The rest is dummy padding.

- How to use it: If the RP2040 parser breaks, count the bytes in this hex dump to ensure your while loop pointer (commands += X) is advancing by the exact same amount.

### 3. The Telemetry Traps
If there is a desynchronization in the payload stride, the custom telemetry traps will catch it:

```Error: ... --- TELEMETRY TRAP --- Host expected 32 bytes, but actually read: 32 bytes```
What it means: This confirms the USB pipeline is healthy. OpenOCD demands exactly 32 bytes back for every syncbb_scan payload it sends. If it reads 32, the RP2040 padded the return buffer correctly. If it reads 30 or 0, there is a calculation error in cmd.c causing a buffer underflow.

### 4. End-of-Chain and BYPASS Warnings
At the end of a loopback or unconfigured hardware test, you might see:

```text
Warn: Unexpected idcode after end of chain
Error: IR capture error; saw 0x00, not 0x01
```

What it means: OpenOCD successfully communicated with the bridge, but the target hardware did not shift out valid JTAG specification bits (like the mandatory 0x01 trailing IR bit). OpenOCD will automatically force the hardware into a 1-bit BYPASS mode for safety, which may cause subsequent deep-register scans to fail. This usually indicates the physical wiring to the target is loose or the target is unpowered.

---

## Deployment & Testing
### 1. Flash the RP2040
- 1 Hold the BOOTSEL button on your RP2040 board and connect it to your PC via USB (or tap the RESET button while holding BOOTSEL if it is already connected).
  2 Drag and drop the newly compiled dirtyJtag.uf2 file onto the RPI-RP2 mass storage drive.
  3 The board will reboot. Watch the onboard LEDs.
     - Blink Off: Indicates the target is currently being flashed autonomously over the internal SPI traces.
     - Solid On: Indicates the boot sequence is complete, the voltage regulators are locked open, and the target is ready.

### 2. Run the Hardware Verification
Once the board is running the custom firmware, use the stripped-down Python diagnostic script to verify the internal SPI routing:

#### Python Diagnostic:
```text
Bash
python flash_fpga.py
```
#### OpenOCD Verification:

```text
Bash
sudo openocd -d3 -f software/dirtyjtag.cfg
```

---
