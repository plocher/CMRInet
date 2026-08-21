import sys
import time
import json
import serial
from pathlib import Path
import _gap_deltas

_DEFAULT_HOST_PORT = "/dev/cu.usbmodem282201"

def sync_and_validate_boot(ser, timeout=15.0):

    start = time.time()
    while time.time() - start < timeout:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
        print(f"BOOT: {line}")
        if line.startswith('{') and '"image"' in line:
            try:
                doc = json.loads(line)
                image = doc.get("image")
                version = doc.get("version")
                if image != "xiao_host_tracer":
                    print(f"ERROR: Expected image 'xiao_host_tracer', got '{image}'. Check flash.", file=sys.stderr)
                    sys.exit(1)
                # simple version check
                if not version:
                    print(f"ERROR: Expected version >= 0.4.0, got '{version}'. Check flash.", file=sys.stderr)
                    sys.exit(1)
                ver_parts = tuple(int(x) for x in version.split('.'))
                if ver_parts < (0, 4, 0):
                    print(f"ERROR: Expected version >= 0.4.0, got '{version}'. Check flash.", file=sys.stderr)
                    sys.exit(1)
                print(f"Verified boot: {image} v{version}")
                return True
            except json.JSONDecodeError:
                pass
    return False

def flush_lines(ser):
    # drain any remaining data
    ser.reset_input_buffer()

def run_combo(ser, s, p, mode, traffic, secs, out_dir, tag):
    print(f"\n--- Running combo: stall={s}ms period={p}ms mode={mode} ---")
    log_file = out_dir / f"{tag}.log"
    
    # Send commands
    ser.write(b"reset\n")
    time.sleep(0.1)
    flush_lines(ser)
    
    # Traffic
    if "fast" in traffic:
        ser.write(b"enable fastwalker\n")
        time.sleep(0.1)
    if "slow" in traffic:
        ser.write(b"enable slowwalker\n")
        time.sleep(0.1)
    if "loopback" in traffic:
        ser.write(b"enable toggleoutfrominput\n")
        time.sleep(0.1)
        
    # Stall
    if s > 0:
        cmd = f"enable stall {s} period {p} mode {mode}\n"
        ser.write(cmd.encode('utf-8'))
        time.sleep(0.1)
        
    flush_lines(ser)
    
    # Run
    cmd = f"run {secs}\n"
    ser.write(cmd.encode('utf-8'))
    
    print(f"Waiting for END CAPTURE (secs={secs})...")
    deadline = time.time() + secs + 5.0
    end_capture_seen = False
    
    while time.time() < deadline:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
        if line.startswith("END CAPTURE"):
            end_capture_seen = True
            print(f"Seen: {line}")
            break
            
    if not end_capture_seen:
        print(f"ERROR_TIMEOUT: END CAPTURE not seen for {tag}")
        return _gap_deltas.AnalyzerResult(
            verdict="ERROR_TIMEOUT", phantom_ua=-1, first_t_ms=-1, last_t_ms=-1
        )
        
    # Dump
    ser.write(b"dump\n")
    print("Dumping ring...")
    deadline = time.time() + 10.0
    
    dump_lines = []
    in_dump = False
    
    while time.time() < deadline:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
            
        if line.startswith("BEGIN DUMP"):
            in_dump = True
        elif line == "END DUMP":
            break
        elif in_dump:
            dump_lines.append(line)
            
    print(f"Captured {len(dump_lines)} lines for {tag}")
    
    import datetime
    with log_file.open("w") as f:
        f.write(f"# CMRI Tracer Capture\n")
        f.write(f"# tag: {tag}\n")
        f.write(f"# stall_ms: {s}\n")
        f.write(f"# period_ms: {p}\n")
        f.write(f"# mode: {mode}\n")
        f.write(f"# traffic: {traffic}\n")
        f.write(f"# secs: {secs}\n")
        f.write(f"# timestamp: {datetime.datetime.now().isoformat()}\n")
        for line in dump_lines:
            f.write(line + "\n")
            
    res = _gap_deltas.analyze_lines(dump_lines)
    
    # Gracefully shut down the run (disable traffic, logging, etc)
    ser.write(b"reset\n")
    time.sleep(0.5)
    flush_lines(ser)
    
    return res

