import json
import re
from pathlib import Path

def analyze_log(filepath):
    frames = []
    with open(filepath, "rb") as f:
        for line in f:
            try:
                doc = json.loads(line.decode('utf-8', errors='replace'))
                if doc.get("event") == "frame":
                    frames.append((doc["ts"], doc["ua"], doc["mt"]))
            except:
                pass
    return frames

def find_gaps(frames, min_gap_ms=500):
    gaps = []
    if not frames:
        return gaps
        
    for i in range(1, len(frames)):
        gap = frames[i][0] - frames[i-1][0]
        if gap >= min_gap_ms:
            gaps.append((frames[i-1], frames[i], gap))
    return gaps

tx_frames = analyze_log("data/results_sync_test/sniffer_tx.log")
rx_frames = analyze_log("data/results_sync_test/sniffer_rx.log")

print(f"TX Sniffer captured {len(tx_frames)} frames")
print(f"RX Sniffer captured {len(rx_frames)} frames")

tx_gaps = find_gaps(tx_frames)
rx_gaps = find_gaps(rx_frames)

print(f"\nTX Sniffer Gaps (>500ms):")
for start, end, gap in tx_gaps:
    print(f"  Gap of {gap}ms between ts={start[0]} ({start[2]}) and ts={end[0]} ({end[2]})")

print(f"\nRX Sniffer Gaps (>500ms):")
for start, end, gap in rx_gaps:
    print(f"  Gap of {gap}ms between ts={start[0]} ({start[2]}) and ts={end[0]} ({end[2]})")
