#!/usr/bin/env python3
"""Simultaneous: TracerHost CDC telemetry + RS485 dongle on Host R+/-.
Proves whether R reaches the Host UART (tracer replies/trace) while the
dongle on Host R+/- sees nothing (the paradox), or resolves it."""
import serial, time, re, sys, threading

import bench_ports

# Defaults resolve from bench.json (#68); argv overrides win:
#   tracer_dongle.py [tracer_port] [dongle_port]
TRACER = sys.argv[1] if len(sys.argv) > 1 else None
DONGLE = sys.argv[2] if len(sys.argv) > 2 else None
TRACER = TRACER or bench_ports.resolve_or_exit("Host")
DONGLE = DONGLE or bench_ports.resolve_or_exit("Dongle")
DWELL = 15.0

STX, ETX, DLE, SYN = 0x02, 0x03, 0x10, 0xFF

def decode_frames(buf):
    i = 0; n = len(buf); out = []; state = "hunt"
    escaped = False; ua = 0; mt = 0; body = bytearray()
    while i < n:
        b = buf[i]
        if escaped:
            body.append(b); escaped = False; i += 1; continue
        if b == DLE: escaped = True; i += 1; continue
        if b == STX: state = "ua"; body = bytearray(); i += 1; continue
        if b == ETX:
            if state == "body":
                out.append((ua, chr(mt) if 0x20<=mt<=0x7e else '?', bytes(body)))
            state = "hunt"; i += 1; continue
        if state == "ua": ua = b; state = "mt"
        elif state == "mt": mt = b; state = "body"
        elif state == "body": body.append(b)
        i += 1
    return out

def tracer_thread(out):
    try:
        s = serial.Serial(TRACER, 115200, timeout=0.1)
        s.dtr = True; s.rts = True
    except Exception as e:
        out.append("OPEN FAILED: %s" % e); return
    t0 = time.time(); buf = bytearray(); sent = 0
    while time.time() - t0 < DWELL:
        if sent < 4 and (time.time() - t0) > 0.5 + sent * 4.0:
            try: s.write(b"status\n")
            except Exception: pass
            sent += 1
        chunk = s.read(4096)
        if chunk: buf.extend(chunk)
    s.close(); out.append(bytes(buf))

def dongle_thread(out):
    try:
        s = serial.Serial(DONGLE, 28800, timeout=0.1, bytesize=8, parity='N', stopbits=2)
        s.dtr = True; s.rts = True
    except Exception as e:
        out.append("OPEN FAILED: %s" % e); return
    t0 = time.time(); buf = bytearray()
    while time.time() - t0 < DWELL:
        chunk = s.read(4096)
        if chunk: buf.extend(chunk)
    s.close(); out.append(bytes(buf))

tbox, dbox = [], []
th = threading.Thread(target=tracer_thread, args=(tbox,))
dh = threading.Thread(target=dongle_thread, args=(dbox,))
th.start(); dh.start(); th.join(); dh.join()

# ---- Tracer report ----
print("===== XIAO HOST TRACER (CDC) =====")
if tbox and isinstance(tbox[0], str):
    print("  " + tbox[0])
else:
    txt = tbox[0].decode("utf-8","replace") if tbox else ""
    print("  bytes=%d" % len(txt))
    images = sorted(set(re.findall(r'"image"\s*:\s*"([^"]+)"', txt)))
    print("  images=%s" % images)
    r_traces = re.findall(r'"event":"trace".*?"dir":"rx".*?"mt":"R"', txt)
    rx_all   = re.findall(r'"event":"trace".*?"dir":"rx"', txt)
    tx_all   = re.findall(r'"event":"trace".*?"dir":"tx"', txt)
    replies  = re.findall(r'"replies"\s*:\s*(\d+)', txt)
    misses   = re.findall(r'"misses"\s*:\s*(\d+)', txt)
    states   = re.findall(r'"state"\s*:\s*"([^"]+)"', txt)
    events   = sorted(set(re.findall(r'"event"\s*:\s*"([^"]+)"', txt)))
    print("  events=%s" % events)
    print("  trace rx(R)=%d  trace rx(all)=%d  trace tx=%d" % (len(r_traces), len(rx_all), len(tx_all)))
    print("  replies=%s  misses=%s  states=%s" % (replies[:4], misses[:4], sorted(set(states))))
    print("  sample lines:")
    for ln in txt[:1200].splitlines()[:10]:
        print("    | " + ln)

# ---- Dongle report ----
print("\n===== RS485 DONGLE on Host R+/- (28800 8N2) =====")
if dbox and isinstance(dbox[0], str):
    print("  " + dbox[0])
else:
    buf = dbox[0] if dbox else b""
    print("  raw bytes=%d" % len(buf))
    if not buf:
        print("  NO SIGNAL on Host R+/-")
    else:
        frames = decode_frames(buf)
        from collections import Counter
        print("  frames decoded=%d  MT=%s  UA=%s" % (len(frames), dict(Counter(f[1] for f in frames)), dict(Counter(f[0] for f in frames))))
        for ua, mt, body in frames[:8]:
            print("    UA=%d MT=%s body=%s" % (ua, mt, body.hex(' ').upper()))
