#!/usr/bin/env python3
import argparse
import serial
import time
import _tracer_client

def main():
    parser = argparse.ArgumentParser(description="Force tracer host into quiescent state")
    parser.add_argument("--port", default=None,
                        help="Host device; default resolves from bench.json")
    args = parser.parse_args()
    args.port = args.port or _tracer_client.host_port()

    print(f"Connecting to {args.port}...")
    ser = serial.Serial(args.port, 115200, timeout=0.5)
    
    print("Sending reset...")
    ser.write(b"reset\n")
    time.sleep(0.5)
    
    print("Flushing buffers...")
    _tracer_client.flush_lines(ser)
    print("Host is quiet.")
    
if __name__ == "__main__":
    main()
