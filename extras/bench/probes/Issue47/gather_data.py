#!/usr/bin/env python3
"""
Harness for #47 backoff-under-loop-stall sweep, using TracerHost over CDC.
Replaces the old run.sh / sweep_47.sh which used compile-time configuration.
"""
import argparse
import sys
import csv
import time
import json
from pathlib import Path
import importlib.util
import serial

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import bench_ports

sys.path.insert(0, str(Path(__file__).parent / "analyzers"))
import importlib
gap_deltas = importlib.import_module("analyze_data")

def sync_and_validate_boot(ser, timeout=15.0):
    ser.write(b"status\n")
    time.sleep(0.5)
    start = time.time()
    while time.time() - start < timeout:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
        print(f"BOOT: {line}")
        if not line.startswith('{'):
            continue
        try:
            doc = json.loads(line)
        except json.JSONDecodeError:
            continue
        if doc.get("event") not in ("status", "epoch"):
            continue
        image = doc.get("image")
        version = doc.get("version")
        if image != "tracer_host":
            print(f"ERROR: Expected image 'tracer_host', got '{image}'. Check flash.", file=sys.stderr)
            sys.exit(1)
        if not version:
            print(f"ERROR: Expected version >= 0.4.0, got '{version}'. Check flash.", file=sys.stderr)
            sys.exit(1)
        ver_parts = tuple(int(x) for x in version.split('.'))
        if ver_parts < (0, 4, 0):
            print(f"ERROR: Expected version >= 0.4.0, got '{version}'. Check flash.", file=sys.stderr)
            sys.exit(1)
        print(f"Verified boot: {image} v{version}")
        return True
    return False

def flush_lines(ser):
    # drain any remaining data
    while ser.in_waiting:
        ser.readline()

def run_combo(ser, s, p, mode, traffic, secs, out_dir, tag):
    print(f"\n--- Running combo: stall={s}ms period={p}ms mode={mode} ---")
    log_file = out_dir / f"{tag}.log"
    
    # Send commands
    ser.write(b"reset\n")
    time.sleep(0.1)
    flush_lines(ser)
    
    # Traffic
    if "fast" in traffic:
        ser.write(b"enable fastwalker UA 30\n")
        time.sleep(0.1)
    if "slow" in traffic:
        ser.write(b"enable slowwalker UA 30\n")
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
        return gap_deltas.AnalyzerResult(
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
    
    with log_file.open("w") as f:
        for line in dump_lines:
            f.write(line + "\n")
            
    res = gap_deltas.analyze_lines(dump_lines)
    return res

def main():
    parser = argparse.ArgumentParser(description="Sweep #47")
    parser.add_argument("--port", default=None,
                        help="Host device; default resolves from bench.json")
    parser.add_argument("--secs", type=int, default=60)
    parser.add_argument("--stalls", default="1 3 5 7 8 9 10 11 12 15 20 30 50 100 250")
    parser.add_argument("--periods", default="125 145 150 155 200 233 250 373 500")
    parser.add_argument("--traffic", default="fast")
    parser.add_argument("--busy", action="store_true")
    parser.add_argument("--yield", dest="mode_yield", action="store_true")
    parser.add_argument("--out", default="sweep_results")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    args.port = args.port or bench_ports.resolve_or_exit("Host")
    
    stalls = [int(x) for x in args.stalls.replace("*", "").split() if x.strip()]
    periods = [int(x) for x in args.periods.replace("*", "").split() if x.strip()]
    
    mode = "busy" if args.busy else "yield"
    import datetime
    out_dir = Path(args.out)
    
    if str(out_dir) == "sweep_results":
        today = datetime.datetime.now().strftime("%Y%m%d")
        base = Path(__file__).parent / "data" / f"results.{today}"
        candidate = base
        count = 1
        while candidate.exists():
            candidate = Path(str(base) + f".{count}")
            count += 1
        out_dir = candidate
        
    print(">>> #47 grid sweep (v2 via CDC capture)")
    print(f"    stalls  : {stalls}")
    print(f"    periods : {periods}")
    
    # Calculate non-skipped combos for dry-run
    active_combos = 0
    for s in stalls:
        for p in periods:
            if p >= 2 * s:
                active_combos += 1
                
    print(f"    combos  : {len(stalls) * len(periods)} total, {active_combos} active (p >= 2*s)")
    print(f"    secs    : {args.secs}")
    print(f"    port    : {args.port}")
    print(f"    mode    : {mode}")
    print(f"    traffic : {args.traffic}")
    
    if args.dry_run:
        print("\nDry run complete. Matrix:")
        for s in stalls:
            for p in periods:
                tag = f"s{s}_p{p}_{mode}"
                if p < 2 * s:
                    print(f"  {tag:15s} -> SKIPPED_REDUNDANT")
                else:
                    print(f"  {tag:15s} -> ACTIVE")
        return 0
        
    out_dir.mkdir(parents=True, exist_ok=True)
    summary_csv = out_dir / "summary.csv"
    
    write_header = not summary_csv.exists()
    
    ser = serial.Serial(args.port, 115200, timeout=0.5)
    time.sleep(2) # boot settle
    
    if not sync_and_validate_boot(ser):
        print("ERROR: Boot validation failed (timeout or wrong image).", file=sys.stderr)
        return 1
        
    print("Configuring topology for session...")
    ser.write(b"node add 30 C 7 7\n")
    time.sleep(0.1)
    ser.write(b"node add 31 C 4 4\n")
    time.sleep(0.1)
    flush_lines(ser)
    
    with summary_csv.open("a", newline="") as f:
        writer = csv.writer(f)
        if write_header:
            writer.writerow([
                "stall_ms", "period_ms", "verdict", "tx_events", "max_gap_ms",
                "median_gap_ms", "p90_gap_ms", "capture", "transcript",
                "verdict_v2", "monotone_prefix_max_ms", "it_count", "poll_count"
            ])
            
        i = 1
        total = len(stalls) * len(periods)
        
        for s in stalls:
            for p in periods:
                tag = f"s{s}_p{p}_{mode}"
                print(f"[{i}/{total}] {tag}")
                
                if p < 2 * s:
                    print(f"  -> SKIPPED_REDUNDANT (p={p} < 2*s={2*s})")
                    writer.writerow([
                        s, p, "SKIPPED_REDUNDANT", 0, 0,
                        0, 0, "", "",
                        "SKIPPED_REDUNDANT", 0, 0, 0
                    ])
                    f.flush()
                    i += 1
                    continue
                
                res = run_combo(ser, s, p, mode, args.traffic, args.secs, out_dir, tag)
                
                # Keep round-1 compat output plus new columns
                writer.writerow([
                    s, p, res.verdict, res.poll_count, res.max_gap, 
                    res.median_gap, res.p90_gap, f"{tag}.log", f"{tag}.txt",
                    res.verdict, res.monotone_prefix_max, res.it_count, res.poll_count
                ])
                f.flush()
                
                print(f"  -> {res.verdict} max_gap={res.max_gap}")
                i += 1
                
    return 0

if __name__ == "__main__":
    sys.exit(main())
