#!/usr/bin/env python3
import argparse
import sys
import time
import threading
from pathlib import Path
import serial

import _tracer_client
import _gap_deltas

def read_sniffer(port_name, out_filename, stop_event):
    try:
        ser = serial.Serial(port_name, 115200, timeout=0.1)
        print(f"[{port_name}] Started logging to {out_filename}")
        with open(out_filename, "wb") as f:
            while not stop_event.is_set():
                if ser.in_waiting:
                    data = ser.read(ser.in_waiting)
                    f.write(data)
                    f.flush()
                else:
                    time.sleep(0.01)
        ser.close()
        print(f"[{port_name}] Finished.")
    except serial.SerialException as e:
        print(f"[{port_name}] ERROR: {e}")

def main():
    parser = argparse.ArgumentParser(description="Sync validation scenario")
    parser.add_argument("--host-port", default=_tracer_client.HOST_PORT)
    parser.add_argument("--sniffer-tx", default=_tracer_client.SNIFFER_TX_PORT)
    parser.add_argument("--sniffer-rx", default=_tracer_client.SNIFFER_RX_PORT)
    args = parser.parse_args()
    
    out_dir = Path("data/results_sync_test")
    out_dir.mkdir(parents=True, exist_ok=True)
    
    print(">>> Sync Validation Scenario")
    print(f"    Host: {args.host_port}")
    print(f"    Sniffer TX: {args.sniffer_tx}")
    print(f"    Sniffer RX: {args.sniffer_rx}")
    
    # Start sniffer captures
    stop_event = threading.Event()
    t_tx = threading.Thread(target=read_sniffer, args=(args.sniffer_tx, out_dir / "sniffer_tx.log", stop_event))
    t_rx = threading.Thread(target=read_sniffer, args=(args.sniffer_rx, out_dir / "sniffer_rx.log", stop_event))
    t_tx.start()
    t_rx.start()
    
    try:
        # Cycle 1: Fast walker only
        print("\n--- Cycle 1: Fast walker only (10s) ---")
        ser = _tracer_client.reboot_and_reconnect(args.host_port)
        if not _tracer_client.sync_and_validate_boot(ser):
            print("ERROR: Boot validation failed.", file=sys.stderr)
            return 1
        ser.write(b"node add 30 C 7 7\n")
        time.sleep(0.1)
        ser.write(b"node add 31 C 4 4\n")
        time.sleep(0.1)
        _tracer_client.flush_lines(ser)
        
        res = _tracer_client.run_combo(ser, 0, 0, "yield", "fast", 10, out_dir, "cycle1_fast")
        print(f"  -> {res.verdict}")
        
        # Inter-cycle 1: Reset (software)
        print("\n--- Inter-cycle 1: Reset (software) ---")
        ser.write(b"reset\n")
        time.sleep(2)
        _tracer_client.flush_lines(ser)
        
        # Cycle 2: Slow walker only
        print("\n--- Cycle 2: Slow walker only (10s) ---")
        res = _tracer_client.run_combo(ser, 0, 0, "yield", "slow", 10, out_dir, "cycle2_slow")
        print(f"  -> {res.verdict}")
        
        # Inter-cycle 2: Reboot (hardware)
        print("\n--- Inter-cycle 2: Reboot (hardware) ---")
        ser.write(b"reboot\n")
        ser.flush()
        ser.close()
        
        # Wait for port to re-enumerate
        time.sleep(2.0)
        start = time.time()
        while time.time() - start < 10.0:
            try:
                ser = serial.Serial(args.host_port, 115200, timeout=0.5)
                break
            except Exception:
                time.sleep(0.5)
        
        if not _tracer_client.sync_and_validate_boot(ser):
            print("ERROR: Boot validation failed after reboot.", file=sys.stderr)
            return 1
            
        ser.write(b"node add 30 C 7 7\n")
        time.sleep(0.1)
        ser.write(b"node add 31 C 4 4\n")
        time.sleep(0.1)
        _tracer_client.flush_lines(ser)
        
        # Cycle 3: Loopback only
        print("\n--- Cycle 3: Loopback only (10s) ---")
        res = _tracer_client.run_combo(ser, 0, 0, "yield", "loopback", 10, out_dir, "cycle3_loopback")
        print(f"  -> {res.verdict}")
        
    finally:
        print("\n--- Cleaning up ---")
        ser.write(b"reset\n")
        time.sleep(0.5)
        _tracer_client.flush_lines(ser)
        ser.close()
        print("Host quiesced.")
        
        print("Stopping sniffers...")
        stop_event.set()
        t_tx.join()
        t_rx.join()
        print("Done.")
        
    return 0

if __name__ == "__main__":
    sys.exit(main())
