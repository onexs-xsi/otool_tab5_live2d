#!/usr/bin/env python3
"""Send commands to the tab5 console over USB-Serial-JTAG (COM3) and print replies."""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"


def main():
    ser = serial.Serial(PORT, 115200, timeout=1.0)
    time.sleep(0.3)
    ser.reset_input_buffer()
    for cmd in ["help", "version", "free", "wifi", "llm-status", "llm-ask", "llm-cancel", "llm-status"]:
        ser.write((cmd + "\n").encode())
        time.sleep(0.8)
        out = ser.read(ser.in_waiting or 1).decode("utf-8", "replace")
        print(f"--- $ {cmd} ---")
        print(out)
    ser.close()


if __name__ == "__main__":
    main()
