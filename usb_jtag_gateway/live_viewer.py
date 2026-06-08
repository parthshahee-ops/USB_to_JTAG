import socket
import serial
import sys

COM_PORT = 'COM10'
BAUD_RATE = 115200
PORT = 7777

print(f"[*] Opening Serial Port {COM_PORT}...")
ser = serial.Serial(COM_PORT, BAUD_RATE, timeout=0.01)

server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server_socket.bind(('127.0.0.1', PORT))
server_socket.listen(1)

print(f"[*] Waiting for OpenOCD connection...")
client_conn, client_addr = server_socket.accept()
print(f"[+] OpenOCD connected!")

try:
    client_conn.setblocking(False)
    
    in_log_mode = False
    in_tdo_mode = False

    while True:
        # 1. Forward data from OpenOCD -> Shrike Lite
        try:
            openocd_data = client_conn.recv(1024)
            if openocd_data:
                ser.write(openocd_data)
        except BlockingIOError:
            pass
        except ConnectionResetError:
            print("\n[-] OpenOCD disconnected.")
            break

        # 2. Read and parse incoming data tokens from Shrike Lite
        if ser.in_waiting > 0:
            raw_bytes = ser.read(ser.in_waiting)
            for byte in raw_bytes:
                char = chr(byte)
                
                if in_log_mode:
                    sys.stdout.write(char)
                    if char == '\n':
                        sys.stdout.flush()
                        in_log_mode = False
                        
                elif in_tdo_mode:
                    # Forward the exact TDO response character back to OpenOCD's network port
                    client_conn.send(char.encode('utf-8'))
                    in_tdo_mode = False
                    
                else:
                    # Check for your custom framing protocol tags
                    if char == '!':
                        in_log_mode = True
                    elif char == '>':
                        in_tdo_mode = True

except KeyboardInterrupt:
    print("\n[*] Exiting...")
finally:
    client_conn.close()
    server_socket.close()
    ser.close()