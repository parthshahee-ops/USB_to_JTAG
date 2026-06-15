import usb.core
import usb.util
import time

# Device discovery
dev = usb.core.find(idVendor=0x1209, idProduct=0xC0CA)
if dev is None:
    raise ValueError("DirtyJTAG device not found.")

try:
    if dev.is_kernel_driver_active(0):
        dev.detach_kernel_driver(0)
except (usb.core.USBError, NotImplementedError):
    pass

try:
    dev.set_configuration()
except usb.core.USBError:
    pass 

cfg  = dev.get_active_configuration()
intf = cfg[(0, 0)]

ep_out = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
ep_in = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)

# ---------------------------------------------------------------------------
# Build the Standard-Compliant TX packet
# ---------------------------------------------------------------------------
PAYLOAD_BYTES = bytes([0xAA, 0x55] * 16) # 32 bytes of data

# Calculate bit length for the DirtyJTAG specification
bit_len = len(PAYLOAD_BYTES) * 8
len_h = (bit_len >> 8) & 0xFF
len_l = bit_len & 0xFF

# Packet Format: [0xEE] [LEN_H] [LEN_L] [DATA...]
tx_packet = bytearray([0xEE, len_h, len_l]) + bytearray(PAYLOAD_BYTES)

print("=" * 50)
print("Standard DirtyJTAG Protocol — SPI Loopback Test")
print("=" * 50)
print(f"Sending Packet   ({len(tx_packet)} bytes): {[hex(b) for b in tx_packet[:6]]} ...")

try:
    # STEP 1: Send the formatted packet
    ep_out.write(tx_packet, timeout=1000)
    time.sleep(0.01)

    # STEP 2: Read ONLY the data bytes back
    # The spec dictates the MCU strips the header and only returns ceil(LEN / 8) bytes.
    expected_return_bytes = len(PAYLOAD_BYTES)
    rx_data = ep_in.read(expected_return_bytes, timeout=1000)

    print(f"Received Payload ({len(rx_data)} bytes): {[hex(b) for b in rx_data[:6]]} ...")
    print("=" * 50)

except usb.core.USBError as e:
    print(f"\n[ERROR] USB transfer failed: {e}")
    raise SystemExit(1)

# ---------------------------------------------------------------------------
# Validation
# Because the MCU stripped the 0xEE header, the first byte of rx_data 
# is the MISO pipeline shift (0x00), and the second byte is the data (0xAA).
# ---------------------------------------------------------------------------

if rx_data[0] == 0x00 and rx_data[1] == 0xAA and rx_data[2] == 0x55:
    print("\n[PASS] FPGA pipeline shift detected.")
    print("       Target hardware is successfully echoing MOSI back on MISO.")
elif bytes(rx_data) == PAYLOAD_BYTES:
    print("\n[PASS] Raw wire loopback detected.")
else:
    print("\n[FAIL] MISO Flatline or corrupted data.")