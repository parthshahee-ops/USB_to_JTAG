import usb.core
import usb.util
import sys

# === HARDWARE CONFIGURATION ===
VID = 0x1209  # DirtyJTAG standard VID
PID = 0xc0ca  # DirtyJTAG standard PID

# Command Constants (Modify to match your specific wrapper if needed)
CMD_SPEED = 0x02
CMD_TRANSFER = 0xee

def main():
    # 1. Initialize USB Connection
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print("Device not found. Is it plugged in and not held by Thonny?")
        sys.exit(1)

    dev.set_configuration()
    cfg = dev.get_active_configuration()
    intf = cfg[(0,0)]
    
    ep_out = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
    ep_in = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)

    # 2. Set Clock Speed to 500 kHz
    # 500 in hex is 0x01F4
    try:
        dev.write(ep_out, [CMD_SPEED, 0x01, 0xF4])
        print("Speed set to 500 kHz.")
    except Exception as e:
        print(f"Failed to set speed: {e}")
        sys.exit(1)

    # ==========================================================
    # FLASHING BLOCK COMPLETELY REMOVED FROM HERE
    # The C-firmware is handling the Verilog configuration now.
    # ==========================================================

    print("\n=== EXECUTING LOOPBACK TEST ===")
    
    # 3. Construct the Test Payload (32 bytes of alternating 0xaa / 0x55)
    test_payload = []
    for i in range(16):
        test_payload.extend([0xaa, 0x55])
        
    # Build the full command frame: [CMD, Len_H, Len_L, Payload...]
    # Length is 32 bytes (0x00, 0x20)
    transfer_cmd = [CMD_TRANSFER, 0x01, 0x00] + test_payload    
    # 4. Fire the Payload
    try:
        dev.write(ep_out, transfer_cmd)
        
        # 5. Read the Echoed Response
        response = dev.read(ep_in, 64, timeout=1000)
        
        print("Echoed Command :", hex(response[0]))
        print("Echoed Len (H) :", hex(response[1]))
        print("Echoed Len (L) :", hex(response[2]))
        
        echoed_data = [hex(x) for x in response[3:35]]
        print("Echoed Payload :", echoed_data)
        print("======================================\n")
        
        if echoed_data[0] == '0xaa':
            print("SUCCESS! FPGA is alive, configured, and routing data!")
        else:
            print("FAILED. Received empty or corrupted data.")
            
    except usb.core.USBError as e:
        print(f"USB Error during transfer: {e}")

if __name__ == '__main__':
    main()