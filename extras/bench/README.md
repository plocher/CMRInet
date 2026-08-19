# extras/bench — RS485 bus probe scripts

Python scripts that talk to the CMRInet bench hardware over USB. They read the sniffer CDC streams and the RS485 dongle to witness what is on the wire. Project-private and experimental.

## Setup

These scripts need `pyserial`. The venv lives inside the repo (gitignored) so it survives reboots. Recreate it with the setup script:

```shell
extras/bench/setup.sh
```

This creates `extras/bench/.venv` and installs `pyserial`. It is idempotent, so re-run it any time the venv is missing or broken.

Run a script with that interpreter:

```shell
extras/bench/.venv/bin/python extras/bench/<script>.py
```

## Ports

The Xiaos enumerate as shuffling `/dev/cu.usbmodem*` names. All three report the same FQBN, so identify a board by behavior, not by the port name:
- a sniffer emits `"image":"xiao_sniffer"` stats every 5 s
- the tracer answers the `status` verb
- SimpleHost stays silent except on a reject

The dongle is `/dev/cu.usbserial-BG04ID4L` on this bench. Confirm with `arduino-cli board list` or `ls /dev/cu.usb*` before each run.

The scripts take port overrides as argv so a reshuffle does not require an edit. Defaults match the 2026-08-18 session.

## Scripts

### dongle_decode.py — calibrated CMRI frame decoder over the dongle

Captures raw bytes from the RS485 dongle at 28800 8N2 with DTR and RTS asserted, and decodes `FF FF STX UA MT <body> ETX` frames with DLE-escape awareness. Prints the UA, MT, and body for each frame, plus MT and UA distributions. Use this to map which physical terminals carry which signal (P/T vs R).

```shell
extras/bench/.venv/bin/python extras/bench/dongle_decode.py "tap label"
```

The label is a free-text tag printed in the header. Pass a second arg to override the port.

### three.py — three-witness simultaneous capture

Opens both Xiao sniffers and the dongle at the same time and captures for 15 s. For each Xiao it reports the `image`, events, MTs, frame count, and the delta of the decoder stats counters. For the dongle it reports raw bytes and decoded frames. Use this for an A/B comparison across host firmware with all three witnesses speaking.

```shell
extras/bench/.venv/bin/python extras/bench/three.py [s1_port] [s2_port] [dongle_port]
```

Xiao #1 should tap the poll pair. Xiao #2 and the dongle should tap the reply pair. Set the taps to match the test before you run it.

### tracer_dongle.py — tracer telemetry and dongle side by side

Captures the XiaoHostTracer CDC stream and the dongle at the same time for 15 s. Injects the `status` verb into the tracer to pull explicit `replies`, `misses`, and `state` counters. Reports tracer `trace rx(R)` count and the dongle frame decode side by side. Use this to check whether R reaches the Host UART while the dongle on Host R± sees a given result.

```shell
extras/bench/.venv/bin/python extras/bench/tracer_dongle.py [tracer_port] [dongle_port]
```

### flash_and_probe.sh — build, program, and verdict in one step

Flashes a host sketch, boots it, captures all three witnesses, and prints a VERDICT block. The verdict names each witness PASS or FAIL and prints a one-line overall summary. No file-content analysis needed — the verdict is in the printed text.

```shell
extras/bench/flash_and_probe.sh [sketch] [host_port]
```

Defaults: sketch `XiaoHostTracer`, port `/dev/cu.usbmodem282201`. Known sketches: `XiaoHostTracer`, `SimpleHost`.

The expected cycle for issue reproduction:
```shell
extras/bench/flash_and_probe.sh XiaoHostTracer   # expect: HEALTHY
extras/bench/flash_and_probe.sh SimpleHost        # expect: BUG REPRODUCED
```

Under SimpleHost the reply-pair witnesses (Xiao #2 and the dongle) are expected to fail. Under XiaoHostTracer all three are expected to pass. The verdict logic encodes these expectations so a human or agent can read the result without parsing probe output.

### verdict.py — verdict parser (used by flash_and_probe.sh)

Reads `three.py` output on stdin and prints the VERDICT block. Standalone use:
```shell
extras/bench/.venv/bin/python extras/bench/three.py | extras/bench/.venv/bin/python extras/bench/verdict.py [sketch]
```

## Findings

The factual record from the 2026-08-18 session is in `docs/sniffer-reply-pair-findings.md`.
