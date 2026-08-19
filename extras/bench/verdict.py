#!/usr/bin/env python3
"""Verdict parser for flash_and_probe.sh. Reads three.py output on stdin,
prints a PASS/FAIL verdict block. No file-content analysis needed by the
operator — the verdict is in the printed text.

Usage: three.py | verdict.py [sketch]
"""
import sys, re

out = sys.stdin.read()
sketch = sys.argv[1] if len(sys.argv) > 1 else "?"

def section(label):
    i = out.find("===== " + label + " =====")
    if i < 0:
        return ""
    j = out.find("\n=====", i + 1)
    return out[i:j] if j > 0 else out[i:]

def xiao_frames(block):
    m = re.search(r'frame_events=(\d+)', block)
    if m:
        return int(m.group(1))
    m = re.findall(r'framesDecoded\s+\d+ -> \d+\s+\(delta \+?(-?\d+)\)', block)
    return int(m[-1]) if m else 0

def dongle_frames(block):
    if "NO SIGNAL" in block or "raw bytes=0" in block:
        return 0
    m = re.search(r'frames decoded=(\d+)', block)
    return int(m.group(1)) if m else 0

s1_n = xiao_frames(section("XIAO_S1"))
s2_n = xiao_frames(section("XIAO_S2"))
dg_n = dongle_frames(section("DONGLE"))

# map issue #41 (CMRIHost poll/transmit starvation) is fixed: every sketch,
# including SimpleHost, is now expected to show reply-pair traffic. This
# script previously special-cased SimpleHost to expect a silent reply pair
# as "the bug"; that expectation is retired. A silent reply pair under any
# sketch is now a regression, not the known-good state.

def pf(sees, expect_ok):
    return "PASS (sees frames)" if (sees > 0) == expect_ok else (
        "FAIL (silent)" if expect_ok else "FAIL (unexpected frames)")

print("Xiao #1 (poll pair):   %s  [frames=%d]" % (pf(s1_n, True), s1_n))
print("Xiao #2 (reply pair):  %s  [frames=%d]" % (pf(s2_n, True), s2_n))
print("Dongle (reply pair):   %s  [frames=%d]" % (pf(dg_n, True), dg_n))
print("")

poll_ok = s1_n > 0
reply_ok = s2_n > 0 or dg_n > 0

if poll_ok and reply_ok:
    print("OVERALL: HEALTHY — both pairs active under %s." % sketch)
elif poll_ok and not reply_ok:
    print("OVERALL: REGRESSION — reply pair deaf under %s (map issue #41 should be fixed)." % sketch)
else:
    print("OVERALL: AMBIGUOUS — poll pair also silent; check bench wiring/power.")
