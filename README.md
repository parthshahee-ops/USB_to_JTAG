# USB-to-JTAG Protocol Gateway

This repository contains the firmware and hardware description design files for a high-speed, hardware-accelerated protocol conversion gateway. The architecture leverages a Raspberry Pi RP2040 microcontroller and a Renesas ForgeFPGA fabric to bridge an external USB-based master device to a standard IEEE 1149.1 JTAG target scan chain.

## 1. System Architectural Concept

### 1.1 The Step-by-Step Data Journey
**a. External Host Board**
It decides what commands to send to the target chip. It writes down these instructions and wraps them in a secure shipping box called a USB Packet to send them down the cable.

**b. RP2040 Microcontroller**
It receives the box, slices it open, and throws away the packaging. It extracts the raw message and translates it into a precise, step-by-step blueprint (bit-vectors).
It sends these blueprints over a fast, 4-lane miniature transit system—the QSPI Bus—straight to the FPGA.

**c. The ForgeFPGA Fabric**
It reads the blueprints coming off the QSPI bus.
It uses its lightning-fast digital reflexes to instantly flip physical electrical pins (TCK, TMS, TDI) up and down, turning those blueprints into perfect electrical "Morse code" pulses for the target chip.

### 1.2 Evaluated Host-Side USB Formats (OpenOCD Alternatives)
Because the exact implementation of the external hardware source can vary, the RP2040 translation layer is being designed to evaluate and potentially support the multiple USB-JTAG driver protocols found in OpenOCD ecosystems:

* **CMSIS-DAP (ARM Standard):** Utilizes standard USB HID or WinUSB bulk transfer pipes with fixed-size structural sequence packets (e.g., using `DAP_JTAG_Sequence` tokens). Highly portable and natively supported in modern microcontroller development setups.
* **FT2232 / MPSSE Emulation (FTDI Standard):** Emulates legacy FTDI hardware protocol layers. The RP2040 parses low-level byte streams containing hardware opcodes (such as direction commands and clock edge preferences) accompanied by dedicated length indicators.
* **ST-LINK (Vendor Proprietary):** Uses encapsulated SCSI mass storage commands over USB bulk endpoints. Highly prevalent on STM32 evaluation targets but carries high software parsing complexity for emulation.

## 2. FPGA Internal Processing

The ForgeFPGA fabric implements three parallel digital logic sub-modules (Stations) running concurrently to handle the incoming QSPI JTAG-data stream:

* **QSPI Receiver:** Monitors the QSPI hardware lines. It uses the QSPI Chip Select (`CS`) line transition as a strict framing token to identify packet boundary limits, collecting data bytes on the fly.
* **Command Parser:** Reads the control tags embedded within our team's custom JTAG format. It splits incoming streams, routing data bits destined for state machine navigation to the `TMS` pipeline and configurations to the `TDI` pipeline.
* **JTAG Engine:** Ingests the separated streams from the parser and sequences them synchronously into raw electrical transitions across the physical device interface.

## 3. Project Roadmap & Task Division

The development plan is divided into multi-stage engineering, steps allowing for incremental feature verification:

### Task 1: OpenOCD Environment Mastery
* **Objective:** Establish a stable PC-to-MCU software communication loop.
* **Deliverables:**
  * Configure OpenOCD environment toolchains and driver scripts.
  * Compile and deploy a reference JTAG implementation (e.g., CMSIS-DAP) onto the RP2040 target silicon.
  * Verify that the PC host operating system successfully enumerates the MCU without protocol or interface errors.

### Task 2: Packet Sniffing & Payload Extraction
* **Objective:** Reverse-engineer the host-side USB packets to create an unpacking matrix.
* **Deliverables:**
  * Capture real-time USB communication transactions using software analyzers (Wireshark + USBPcap).
  * Map hexadecimal fields down to individual control parameters based on open-source specifications (CMSIS-DAP / FTDI MPSSE).
  * Implement an initial firmware function on the RP2040 to safely isolate and extract raw data payloads from incoming buffers.

### Task 3: Translation and QSPI Packaging
* **Objective:** Map host command packets to internal hardware logic and transmit over the serial bus.
* **Deliverables:**
  * Write the translation firmware on the RP2040 to turn extracted USB frames into the team's custom JTAG bit-vector layout.
  * Implement the hardware-level QSPI Master driver on the RP2040 to push formatted command packets to the FPGA.
  * Ensure strict timing synchronization between the data stream and the QSPI Chip Select (`CS`) line transitions.

### Task 4: Validation, Loopback, and Debugging
* **Objective:** Perform incremental hardware verification before deploying to a live system.
* **Deliverables:**
  * **Phase A:** Inspect output waveforms from the RP2040 pins using a digital logic analyzer to confirm formatting compliance.
  * **Phase B:** Connect to the ForgeFPGA and implement a temporary internal loopback path (routing output registers straight back to the inbound stream) to verify the data returns to the PC uncorrupted.
  * **Phase C:** Disconnect loopback frameworks and validate the gateway against a live, physical target chip.
 
  ---
