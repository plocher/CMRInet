import json
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

tx_frames = analyze_log("data/results_sync_test/sniffer_tx.log")

# Look at the gaps between P frames for UA 96
p_96 = [f[0] for f in tx_frames if f[1] == 96 and f[2] == 'P']
gaps = [b - a for a, b in zip(p_96, p_96[1:])]
print(f"Gaps for UA 96 P frames from sniffer: {gaps[:20]}")
