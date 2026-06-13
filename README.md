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
│   ├── cmd.c                       # Custom 0xee SPI payload routing logic
│   ├── dirtyJtagConfig.h           # Remapped power pins (28/29)
│   └── bitstream.h                 # Embedded target logic array
├── fpga_code/                       # Hardware Description Logic
│   ├── main.v                      # Verified SPI protocol core
├── software/                       # Host-Side Execution
│   └── flash_fpga.py               # Python payload delivery and SPI diagnostic script
└── README.md
```

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

## Deployment & Testing
1. Flash the RP2040
- 1 Hold the BOOTSEL button on your RP2040 board and connect it to your PC via USB (or tap the RESET button while holding BOOTSEL if it is already connected).
  2 Drag and drop the newly compiled dirtyJtag.uf2 file onto the RPI-RP2 mass storage drive.
  3 The board will reboot. Watch the onboard LEDs.
     - Blink Off: Indicates the target is currently being flashed autonomously over the internal SPI traces.
     - Solid On: Indicates the boot sequence is complete, the voltage regulators are locked open, and the target is ready.

2. Run the Hardware Verification
Once the board is running the custom firmware, use the stripped-down Python diagnostic script to verify the internal SPI routing:

```text
Bash
python flash_fpga.py
```

---
