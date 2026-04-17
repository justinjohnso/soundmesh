import serial
import time
import sys

port = sys.argv[1]
baud = 115200
duration = 15

try:
    ser = serial.Serial(port, baud, timeout=1)
    # Toggle DTR/RTS to reset the board
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    ser.setDTR(True)
    
    start_time = time.time()
    while time.time() - start_time < duration:
        if ser.in_waiting:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if line:
                print(line)
    ser.close()
except Exception as e:
    print(f"Error: {e}")
    sys.exit(1)
