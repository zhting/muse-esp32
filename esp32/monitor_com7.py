import serial
import time
import sys

log_file_path = r"h:\project\muse\muse-gadget-sdk\esp32\serial_stream.log"

try:
    ser = serial.Serial()
    ser.port = 'COM7'
    ser.baudrate = 115200
    ser.dtr = False
    ser.rts = False
    ser.timeout = 1
    ser.open()
    print("Serial COM7 opened without reset, monitoring...")
    with open(log_file_path, "a", encoding="utf-8", buffering=1) as f:
        f.write(f"\n--- Serial Monitor Session Started at {time.ctime()} ---\n")
        while True:
            line = ser.readline()
            if line:
                try:
                    decoded = line.decode('utf-8', errors='replace')
                    print(decoded, end='', flush=True)
                    f.write(decoded)
                except Exception:
                    pass
except Exception as e:
    print(f"Error opening or reading COM7: {e}")
