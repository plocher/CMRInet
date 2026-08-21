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

# Find the exact timestamps of the reboot silence
# We know the reboot happens between cycle 2 and cycle 3
# Let's find the largest gap in the log
gaps = []
for i in range(1, len(tx_frames)):
    gap = tx_frames[i][0] - tx_frames[i-1][0]
    gaps.append((gap, tx_frames[i-1], tx_frames[i]))

gaps.sort(key=lambda x: x[0], reverse=True)
print("Largest gaps in TX sniffer log:")
for gap, start, end in gaps[:5]:
    print(f"  Gap of {gap}ms between ts={start[0]} ({start[2]}) and ts={end[0]} ({end[2]})")
