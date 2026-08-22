import sys
import time
import json
import serial
from pathlib import Path
import _gap_deltas

# Role-based USB port mapping (FIXME: #68 dynamic discovery)
HOST_PORT = "/dev/cu.usbmodem282201"
SNIFFER_TX_PORT = "/dev/cu.usbmodem28101"
SNIFFER_RX_PORT = "/dev/cu.usbmodem2821301"

# Backward compatibility for scripts that still reference _DEFAULT_HOST_PORT
_DEFAULT_HOST_PORT = HOST_PORT

def reboot_and_reconnect(port, timeout=10.0):
    print(f"Rebooting host on {port}...")
    try:
        ser = serial.Serial(port, 115200, timeout=0.5)
        ser.write(b"reboot\n")
        ser.flush()
        time.sleep(0.5)
        ser.close()
    except Exception:
        pass
        
    # Wait for the port to drop and reappear
    time.sleep(2.0)
    
    start = time.time()
    while time.time() - start < timeout:
        try:
            # Busy loop trying to open the port
            ser = serial.Serial(port, 115200, timeout=0.5)
            print("Port re-enumerated.")
            time.sleep(2.0) # Allow Arduino to settle after CDC open
            return ser
        except Exception:
            time.sleep(0.5)
            
    raise serial.SerialException("Failed to reconnect after reboot")



def sync_and_validate_boot(ser, timeout=15.0):
    # Send a status to ask for identity directly (works on fresh boot without nodes)
    ser.write(b"status\n")
    time.sleep(0.5)
    
    start = time.time()
    while time.time() - start < timeout:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
        print(f"BOOT: {line}")
        if line.startswith('{') and '"image"' in line:
            try:
                # Use regex to safely extract image and version even if the line is truncated
                import re
                img_match = re.search(r'"image":"([^"]+)"', line)
                ver_match = re.search(r'"version":"([^"]+)"', line)
                image = img_match.group(1) if img_match else None
                version = ver_match.group(1) if ver_match else None
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
            except Exception:
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
    dstring=""
    dstringsep="t:"
    if "fast" in traffic:
        ser.write(b"enable fastwalker\n")
        dstring += f"{dstringsep}f"
        dstringsep = ", "
        time.sleep(0.1)
    if "slow" in traffic:
        ser.write(b"enable slowwalker\n")
        dstring += f"{dstringsep}s"
        dstringsep = ", "
        time.sleep(0.1)
    if "loopback" in traffic:
        ser.write(b"enable toggleoutfrominput\n")
        dstring += f"{dstringsep}l"
        dstringsep = ", "
        time.sleep(0.1)
    if dstring:
        ser.write(f"display 1 {dstring}\n".encode('utf-8'))
        time.sleep(0.1)
    # Stall
    if s > 0:
        cmd = f"enable stall {s} period {p} mode {mode}\n"
        ser.write(cmd.encode('utf-8'))
        dstring = f"Run: stall:{s}/{p}/{mode}"
        ser.write(b"display 2 " + dstring.encode('utf-8') + b"\n")
        time.sleep(0.1)

    flush_lines(ser)
    
    # Run
    time.sleep(0.1)
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
        ser.write(b"display 2 " + "run ERROR_TIMEOUT".encode('utf-8') + b"\n")
        time.sleep(0.1)

        return _gap_deltas.AnalyzerResult(
            verdict="ERROR_TIMEOUT", phantom_ua=-1, first_t_ms=-1, last_t_ms=-1
        )
    ser.write(b"display 2 " + "run complete".encode('utf-8') + b"\n")
    time.sleep(0.1)

    # Dump

    ser.write(b"display 2 " + "dumping ring".encode('utf-8') + b"\n")
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
    ser.write(b"display 2 " + f"dumped {len(dump_lines)} lines".encode('utf-8') + b"\n")

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
    ser.write(b"display 2 " + "resetting".encode('utf-8') + b"\n")
    ser.write(b"reset\n")
    time.sleep(0.5)
    flush_lines(ser)
    
    return res

