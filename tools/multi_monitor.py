import serial
import threading
import time
import sys

ports = {
    'SRC': '/dev/cu.usbmodem21401',
    'OUT1': '/dev/cu.usbmodem211101',
    'OUT2': '/dev/cu.usbmodem211201',
    'OUT3': '/dev/cu.usbmodem211301'
}

def read_port(name, port):
    try:
        s = serial.Serial(port, 115200, timeout=1)
        while True:
            line = s.readline()
            if line:
                try:
                    decoded = line.decode('utf-8', errors='replace').strip()
                    if decoded:
                        print(f"[{name}] {decoded}")
                except:
                    pass
    except Exception as e:
        print(f"[{name}] Error: {e}")

threads = []
for name, port in ports.items():
    t = threading.Thread(target=read_port, args=(name, port))
    t.daemon = True
    t.start()
    threads.append(t)

try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    sys.exit(0)
