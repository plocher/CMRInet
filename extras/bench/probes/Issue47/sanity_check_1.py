import _tracer_client
import _gap_deltas
import serial
import time

def run_interactive_combo(ser, s, p, mode, traffic, secs):
    print(f"Running combo: stall={s}ms period={p}ms mode={mode}")
    ser.write(b"reset\n")
    time.sleep(0.1)
    _tracer_client.flush_lines(ser)
    
    if "fast" in traffic:
        ser.write(b"enable fastwalker\n")
    if "slow" in traffic:
        ser.write(b"enable slowwalker\n")
    if "loopback" in traffic:
        ser.write(b"enable toggleoutfrominput\n")
        
    if s > 0:
        cmd = f"enable stall {s} period {p} mode {mode}\n"
        ser.write(cmd.encode('utf-8'))
        time.sleep(0.1)
        
    cmd = f"run {secs}\n"
    ser.write(cmd.encode('utf-8'))
    
    print(f"Waiting for END CAPTURE (secs={secs})...")
    deadline = time.time() + secs + 5.0
    while time.time() < deadline:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if line.startswith("END CAPTURE"):
            print(f"Seen: {line}")
            break
            
    ser.write(b"dump\n")
    deadline = time.time() + 10.0
    dump_lines = []
    in_dump = False
    while time.time() < deadline:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if not line: continue
        if line.startswith("BEGIN DUMP"):
            in_dump = True
        elif line == "END DUMP":
            break
        elif in_dump:
            dump_lines.append(line)
            
    res = _gap_deltas.analyze_lines(dump_lines)
    print(f"Verdict: {res.verdict} max_gap={res.max_gap}")
    return res

def main():
    print("Sanity Check 1: Verify Reboot Resets State")
    ser = _tracer_client.reboot_and_reconnect(_tracer_client.HOST_PORT)
    
    if not _tracer_client.sync_and_validate_boot(ser):
        print("ERROR: Boot validation failed.", file=sys.stderr)
        return 1
        
    ser.write(b"node add 30 C 7 7\n")
    time.sleep(0.1)
    ser.write(b"node add 31 C 4 4\n")
    time.sleep(0.1)
    _tracer_client.flush_lines(ser)
    
    print("\n--- Combo 1: stall=20, period=150, mode=yield ---")
    res1 = run_interactive_combo(ser, 20, 150, "yield", "fast slow loopback", 30)
    
    print("\n--- Rebooting... ---")
    ser.write(b"reboot\n")
    ser.flush()
    ser.close()
    time.sleep(2.0)
    
    ser = _tracer_client.reboot_and_reconnect(_tracer_client.HOST_PORT)
    if not _tracer_client.sync_and_validate_boot(ser):
        print("ERROR: Boot validation failed after reboot.", file=sys.stderr)
        return 1
        
    ser.write(b"node add 30 C 7 7\n")
    time.sleep(0.1)
    ser.write(b"node add 31 C 4 4\n")
    time.sleep(0.1)
    _tracer_client.flush_lines(ser)
    
    print("\n--- Combo 2: stall=1, period=150, mode=yield ---")
    res2 = run_interactive_combo(ser, 1, 150, "yield", "fast slow loopback", 30)
    
    print("\n--- Summary ---")
    print(f"Combo 1 (stall=20): {res1.verdict} max_gap={res1.max_gap}")
    print(f"Combo 2 (stall=1): {res2.verdict} max_gap={res2.max_gap}")
    
    ser.close()

if __name__ == "__main__":
    main()
