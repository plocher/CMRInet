#!/usr/bin/env python3
"""Calibrated CMRI frame decoder over the RS485 dongle.
Captures at 28800 8N2 (DTR=RTS=True), decodes FF FF STX UA MT <body> ETX frames
with DLE-escape awareness, and prints UA/MT/body per frame. Use to map which
physical terminals carry which signal (P/T vs R)."""
import serial, time, sys

# Defaults; override with argv: dongle_decode.py [label] [port]
PORT = "/dev/cu.usbserial-BG04ID4L"
BAUD = 28800
DWELL = 10.0
STX, ETX, DLE, SYN = 0x02, 0x03, 0x10, 0xFF

def decode_frames(buf):
    """Yield (ua, mt, body_bytes) for each valid frame in buf."""
    i = 0; n = len(buf); out = []
    state = "hunt"  # hunt -> ua -> mt -> body
    escaped = False; ua = 0; mt = 0; body = bytearray()
    while i < n:
        b = buf[i]
        if escaped:
            body.append(b); escaped = False; i += 1; continue
        if b == DLE:
            escaped = True; i += 1; continue
        if b == STX:
            state = "ua"; body = bytearray(); i += 1; continue
        if b == ETX:
            if state == "body":
                out.append((ua, chr(mt) if 0x20<=mt<=0x7e else '?', bytes(body)))
            state = "hunt"; i += 1; continue
        if state == "ua":
            ua = b; state = "mt"
        elif state == "mt":
            mt = b; state = "body"
        elif state == "body":
            body.append(b)
        i += 1
    return out

label = sys.argv[1] if len(sys.argv) > 1 else "(unlabeled)"
if len(sys.argv) > 2:
    PORT = sys.argv[2]
print(f"===== dongle decoder @ {BAUD} 8N2, {DWELL}s  tap={label} =====")
try:
    s = serial.Serial(PORT, BAUD, timeout=0.2, bytesize=8, parity='N', stopbits=2)
    s.dtr = True; s.rts = True
except Exception as e:
    print(f"OPEN FAILED: {type(e).__name__}: {e}"); sys.exit(1)
t0 = time.time(); buf = bytearray()
while time.time() - t0 < DWELL:
    chunk = s.read(4096)
    if chunk: buf.extend(chunk)
s.close()
print(f"raw bytes captured: {len(buf)}")
if not buf:
    print("NO SIGNAL on this tap."); sys.exit(0)
frames = decode_frames(buf)
from collections import Counter
mt_count = Counter(f[1] for f in frames)
ua_count = Counter(f[0] for f in frames)
print(f"frames decoded: {len(frames)}")
print(f"MT distribution: {dict(mt_count)}")
print(f"UA distribution: {dict(ua_count)}")
print("first 12 frames:")
for ua, mt, body in frames[:12]:
    print(f"  UA={ua:>3} MT={mt} body={body.hex(' ').upper()}")
