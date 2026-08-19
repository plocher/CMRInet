#!/usr/bin/env python3
"""Three-witness simultaneous capture: Xiao sniffer #1, Xiao sniffer #2, dongle.
Reports each witness's verdict (frames decoded / raw bytes / frozen counters)."""
import serial, time, re, sys, threading
from collections import Counter

# Witness ports and their bus-side role (set by the operator's wiring).
# Override with argv: three.py [s1_port] [s2_port] [dongle_port]
S1_PORT = "/dev/cu.usbmodem28101"      # Xiao sniffer #1
S2_PORT = "/dev/cu.usbmodem2821301"    # Xiao sniffer #2
DONGLE  = "/dev/cu.usbserial-BG04ID4L" # RS485 dongle (raw, 28800 8N2)
DWELL = 15.0
if len(sys.argv) > 1: S1_PORT = sys.argv[1]
if len(sys.argv) > 2: S2_PORT = sys.argv[2]
if len(sys.argv) > 3: DONGLE  = sys.argv[3]

STX, ETX, DLE, SYN = 0x02, 0x03, 0x10, 0xFF

def decode_frames(buf):
    i = 0; n = len(buf); out = []; state = "hunt"
    escaped = False; ua = 0; mt = 0; body = bytearray()
    while i < n:
        b = buf[i]
        if escaped: body.append(b); escaped = False; i += 1; continue
        if b == DLE: escaped = True; i += 1; continue
        if b == STX: state = "ua"; body = bytearray(); i += 1; continue
        if b == ETX:
            if state == "body": out.append((ua, chr(mt) if 0x20<=mt<=0x7e else '?', bytes(body)))
            state = "hunt"; i += 1; continue
        if state == "ua": ua = b; state = "mt"
        elif state == "mt": mt = b; state = "body"
        elif state == "body": body.append(b)
        i += 1
    return out

def xiao_capture(port, out, label):
    try:
        s = serial.Serial(port, 115200, timeout=0.1)
        s.dtr = True; s.rts = True
    except Exception as e:
        out.append((label, "OPEN FAILED: %s" % e)); return
    t0 = time.time(); buf = bytearray()
    while time.time() - t0 < DWELL:
        chunk = s.read(4096)
        if chunk: buf.extend(chunk)
    s.close(); out.append((label, bytes(buf)))

def dongle_capture(port, out, label):
    try:
        s = serial.Serial(port, 28800, timeout=0.1, bytesize=8, parity='N', stopbits=2)
        s.dtr = True; s.rts = True
    except Exception as e:
        out.append((label, "OPEN FAILED: %s" % e)); return
    t0 = time.time(); buf = bytearray()
    while time.time() - t0 < DWELL:
        chunk = s.read(4096)
        if chunk: buf.extend(chunk)
    s.close(); out.append((label, bytes(buf)))

results = []
threads = [
    threading.Thread(target=xiao_capture, args=(S1_PORT, results, "XIAO_S1")),
    threading.Thread(target=xiao_capture, args=(S2_PORT, results, "XIAO_S2")),
    threading.Thread(target=dongle_capture, args=(DONGLE, results, "DONGLE")),
]
for t in threads: t.start()
for t in threads: t.join()

# Report in consistent order
order = ["XIAO_S1", "XIAO_S2", "DONGLE"]
results.sort(key=lambda x: order.index(x[0]) if x[0] in order else 99)

for label, data in results:
    print(f"\n===== {label} =====")
    if isinstance(data, str):
        print(f"  {data}"); continue
    if label.startswith("XIAO"):
        txt = data.decode("utf-8", "replace")
        images = sorted(set(re.findall(r'"image"\s*:\s*"([^"]+)"', txt)))
        events = sorted(set(re.findall(r'"event"\s*:\s*"([^"]+)"', txt)))
        mts = sorted(set(re.findall(r'"mt"\s*:\s*"([^"]+)"', txt)))
        frames = len(re.findall(r'"event":"frame"', txt))
        stats_lines = []
        for ln in txt.splitlines():
            if '"event":"stats"' in ln:
                d = {}
                for f in ["framesDecoded","framesRestarted","timeoutAborts","slowGaps","maxGapMs","seq"]:
                    m = re.search(r'"%s"\s*:\s*(\d+)' % f, ln)
                    if m: d[f] = int(m.group(1))
                if d: stats_lines.append(d)
        print(f"  bytes={len(data)}  image={images}")
        print(f"  events={events}  mts={mts}  frame_events={frames}")
        if len(stats_lines) >= 2:
            a, b = stats_lines[0], stats_lines[-1]
            print(f"  stats delta over {DWELL:.0f}s:")
            for f in ["seq","framesDecoded","framesRestarted","timeoutAborts","slowGaps","maxGapMs"]:
                va, vb = a.get(f), b.get(f)
                if va is not None and vb is not None:
                    print(f"    {f:18s} {va} -> {vb}  (delta {vb-va:+d})")
        elif stats_lines:
            print(f"  one stats line: {stats_lines[0]}")
        else:
            print("  no stats lines captured")
    else:  # DONGLE
        print(f"  raw bytes={len(data)}")
        if not data:
            print("  NO SIGNAL (0 bytes)")
        else:
            frames = decode_frames(data)
            nsync = sum(1 for b in data if b==SYN)
            nstx = sum(1 for b in data if b==STX)
            starts = sum(1 for i in range(len(data)-2) if data[i]==SYN and data[i+1]==SYN and data[i+2]==STX)
            print(f"  FF FF STX starts={starts}  frames decoded={len(frames)}")
            if frames:
                print(f"  MT dist={dict(Counter(f[1] for f in frames))}  UA dist={dict(Counter(f[0] for f in frames))}")
                for ua, mt, body in frames[:6]:
                    print(f"    UA={ua} MT={mt} body={body.hex(' ').upper()}")
            print(f"  first 80 bytes hex: {data[:80].hex(' ')}")
