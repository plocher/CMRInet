# Sniffer reply-pair findings — 2026-08-18
This is a factual record of a diagnosis session. It captures what the instruments showed. It does not draw a conclusion. Project-private and experimental.

## Setup
The bench has four devices on one 4-wire RS485 bus at 28800 baud, 8 data bits, 2 stop bits:
- one Host board (Seeed XIAO ESP32-C6 + MAX3491)
- one Node board (same hardware)
- two sniffer boards (same hardware, running `XiaoSniffer.ino` v0.2.1)
- one USB-RS485 dongle (`/dev/cu.usbserial-BG04ID4L`), an independent witness with no CMRInet code

The bus wiring is a crossover. Host T± routes to Node R±. Host R± routes to Node T±.

The three witnesses and their ports:
- **Xiao #1** — `/dev/cu.usbmodem28101` — sniffer firmware, JSON lines over USB CDC
- **Xiao #2** — `/dev/cu.usbmodem2821301` — sniffer firmware, JSON lines over USB CDC
- **dongle** — `/dev/cu.usbserial-BG04ID4L` — raw wire bytes at 28800 8N2

The sniffer firmware uses the library `CMRIFrameDecoder`. This is the same decoder the Host transport uses. The sniffer emits an `epoch` line at boot, a `frame` line for each decoded CMRI frame, and a `stats` line every 5 s. The `stats` line carries decoder health counters: `framesDecoded`, `framesRestarted`, `timeoutAborts`, `slowGaps`, `maxGapMs`.

The two host sketches used in this session:
- **SimpleHost** — `examples/SimpleHost/SimpleHost.ino`. Polls the node and runs a bitwalk on the outputs. Stays silent on USB CDC except on a reject. The bitwalk runs only when the node state is `kOnline`.
- **XiaoHostTracer** — `examples/XiaoHostTracer/XiaoHostTracer.ino`. Polls the node and emits JSON telemetry on USB CDC. Answers the `status` verb with counters including `replies`, `misses`, and `state`. Emits `trace` events for each packet sent and received.

Both host sketches use the same RS485 port: `Esp32UartCMRISerialPort` on pin D3 for TXEN, with Serial1 at 28800 8N2.

## How the witnesses were read
The Xiao sniffers gate their USB CDC output on `Serial` being true (DTR asserted). A plain `cat` of the device that does not assert DTR sees nothing. All probes in this session opened the ports with pyserial at 115200 baud and asserted DTR and RTS.

The dongle gates its receiver on the control lines. The dongle produced data at 28800 8N2 with DTR and RTS both asserted. With other combinations it produced zero bytes.

## Tap positions
The three witnesses were moved between tap points during the session. Each test below names the tap point for each witness. The two tap points are:
- **poll pair** — the pair that carries Host-to-Node traffic (I, T, P). This is Host T± and Node R±.
- **reply pair** — the pair that carries Node-to-Host traffic (R). This is Host R± and Node T±.

The dongle tap for the decisive tests was on the two wires at the Host board R+ and R− screw terminals. These same two wires continue through the crossover cable to the Node board T+ and T− screw terminals.

## Test 1 — poll pair, both Xiao sniffers, SimpleHost
- Host firmware: SimpleHost
- Xiao #1 tap: poll pair
- Xiao #2 tap: reply pair (Host R±)
- dongle: not connected yet

Result over 30 s:
- Xiao #1: `framesDecoded` +180. MT distribution: P and T. UA distribution: 95 (T) and 96 (P).
- Xiao #2: `framesDecoded` +0. All counters frozen. `frame` events: 0.

The SimpleHost LEDs kept bitwalking during this test.

## Test 2 — poll pair validation, dongle
- Host firmware: SimpleHost
- Xiao #1 tap: poll pair
- Xiao #2 tap: reply pair
- dongle tap: poll pair

Result over 10 s:
- dongle: 21 frames decoded. MT distribution: T=10, P=11. UA distribution: 95=10, 96=11. Body bytes show the SimpleHost walking-one pattern.

The dongle and Xiao #1 agreed on the frames.

## Test 3 — dongle on Host R±, SimpleHost
- Host firmware: SimpleHost
- dongle tap: Host R+ and R− screw terminals (reply pair)

Result over 10 s at 28800 8N2:
- dongle: 0 bytes. No signal.

## Test 4 — dongle on Node T±, SimpleHost
- Host firmware: SimpleHost
- dongle tap: Node T+ and T− screw terminals (reply pair)

Result over 10 s at 28800 8N2, all four DTR/RTS combinations:
- dongle: 0 bytes for every combination.

## Test 5 — tracer and dongle, Host R±, XiaoHostTracer
- Host firmware: XiaoHostTracer
- Xiao #1 tap: poll pair
- Xiao #2 tap: reply pair (Host R±) — counters not captured in this run
- dongle tap: Host R+ and R− screw terminals (reply pair)

Result over 15 s:
- tracer CDC: `trace rx(R)` = 808. `replies` counter climbs 0 → 1 → 2. Node state: ONLINE. Sample received frame: UA 95, MT R, body `00 00 00 03 FF 00 01`.
- dongle: 809 frames decoded. MT distribution: R=809. UA distribution: 95=809. Body: `00 00 00 03 FF 00 01`.

The tracer and the dongle agreed to the same frame bytes.

## Test 6 — all three witnesses, Host R±, XiaoHostTracer (gap fill)
- Host firmware: XiaoHostTracer
- Xiao #1 tap: poll pair
- Xiao #2 tap: reply pair (Host R±)
- dongle tap: Host R+ and R− screw terminals (reply pair)

Result over 15 s:
- Xiao #1: `framesDecoded` +2. MT distribution: P. `frame` events: 3.
- Xiao #2: `framesDecoded` +1. MT distribution: R. `frame` events: 3. Counters not frozen.
- dongle: 3 frames decoded. MT distribution: R=3. UA distribution: 95=3. Body: `00 00 00 03 FF 00 01`.

All three witnesses decoded frames under XiaoHostTracer.

## Test 7 — all three witnesses, Host R±, SimpleHost (A/B/A return)
- Host firmware: SimpleHost
- Xiao #1 tap: poll pair
- Xiao #2 tap: reply pair (Host R±)
- dongle tap: Host R+ and R− screw terminals (reply pair)

Result over 15 s:
- Xiao #1: `framesDecoded` +70. MT distribution: P and T. `frame` events: 106.
- Xiao #2: `framesDecoded` +0. All counters frozen. `frame` events: 0.
- dongle: 0 bytes. No signal.

## Tests not run
- The dongle on Node T± under XiaoHostTracer was not run. The facts do not show whether R reaches the Node end of the reply pair under the tracer.
- The polarity inversion test was run earlier in the session by the operator. Inverting the polarity produced errors, which confirms that the current polarity is the driven one. The exact byte captures from that test are not in this record.

## Controlled variables
The decisive comparison is Test 5 (XiaoHostTracer) against Test 7 (SimpleHost). Between these two runs:
- the dongle tap stayed on Host R+ and R− screw terminals
- the Xiao #2 tap stayed on Host R± (reply pair)
- the baud and framing stayed at 28800 8N2
- the bus wiring did not change

The only changes were the Host firmware and the Host power cycle that flashing requires.

The result changed from 809 R frames (tracer) to 0 bytes (SimpleHost) on the dongle, and from `framesDecoded` +1 (tracer) to +0 (SimpleHost) on Xiao #2.

## Summary table
| Test | Host firmware | Xiao #1 (poll) | Xiao #2 (reply) | Dongle (reply) |
|------|--------------|----------------|-----------------|----------------|
| 1 | SimpleHost | +180 P/T | frozen | — |
| 2 | SimpleHost | — | — | 21 P/T (poll pair) |
| 3 | SimpleHost | — | — | 0 bytes |
| 4 | SimpleHost | — | — | 0 bytes (Node T±) |
| 5 | XiaoHostTracer | — | not captured | 809 R |
| 6 | XiaoHostTracer | +2 P | +1 R | 3 R |
| 7 | SimpleHost | +70 P/T | frozen | 0 bytes |

## Facts that rule out causes
- The sniffer firmware is not the cause. The dongle has no CMRInet code and shows the same effect. Xiao #2 decoded R under the tracer (Test 6).
- The Node baud is not the cause. The Host decodes R at 28800 under the tracer. A baud mismatch would put the Node offline.
- The reply pair is not dead. It carries R under the tracer (Tests 5 and 6).
- Polarity is not the cause. Inverting the polarity produced errors, which confirms the current polarity is correct.
- A bad sniffer board is not the cause. Xiao #2 decoded R under the tracer on the same tap where it froze under SimpleHost.

## The open fact
Under SimpleHost, the Host receives R (the node stays online and the bitwalk runs), but the dongle on Host R± sees no signal and Xiao #2 on Host R± shows frozen counters. Under XiaoHostTracer, the same taps see R. The Host firmware is the only controlled variable that changed.

## How to reproduce
One-time setup: create the bench probe venv (see `extras/bench/setup.sh` and `extras/bench/README.md`). The two sniffers and the dongle must be wired to the bus with Xiao #1 on the poll pair and Xiao #2 and the dongle on the reply pair.

Run the two-command A/B sequence from the repo root:
```shell
extras/bench/flash_and_probe.sh SimpleHost
extras/bench/flash_and_probe.sh XiaoHostTracer
```

Each command compiles the named sketch, uploads it to the Host board, boots it, captures all three witnesses for 15 s, and prints a VERDICT block. No file-content analysis is needed. The verdict names each witness PASS or FAIL and prints a one-line overall summary.

Expected output under SimpleHost:
```
=================== VERDICT (SimpleHost) ===================
Xiao #1 (poll pair):   PASS (sees frames)  [frames=N]
Xiao #2 (reply pair):  FAIL (silent — expected, the bug)  [frames=0]
Dongle (reply pair):   FAIL (silent — expected, the bug)  [frames=0]

OVERALL: BUG REPRODUCED — poll pair active, reply pair deaf under SimpleHost.
======================================================
```

Expected output under XiaoHostTracer:
```
=================== VERDICT (XiaoHostTracer) ===================
Xiao #1 (poll pair):   PASS (sees frames)  [frames=N]
Xiao #2 (reply pair):  PASS (sees frames)  [frames=N]
Dongle (reply pair):   PASS (sees frames)  [frames=N]

OVERALL: HEALTHY — both pairs active under XiaoHostTracer.
======================================================
```

For an A/B/A confirmation, run the SimpleHost command a second time. The deafness should return. The Host board port defaults to `/dev/cu.usbmodem282201`; override it as the second argument if the port name changed. See `docs/testbed-physical-notes.md` for the manual arduino-cli compile and upload recipe.

## Resolution — 2026-08-19
Root cause: `CMRIHost::runSchedule_` unconditionally sent a full `T` instead of a `P` whenever a node's `outputsDirty_` flag was set, with no bound on how long a poll could be deferred. SimpleHost's demo bitwalk sets an output bit on a fixed timer; node 31 sitting in the poll table with no physical hardware behind it stretched the round-robin's per-cycle time to roughly the bitwalk's own period (dominated by node 31's 250 ms reply-gate timeout on every pass). That near-resonance meant node 30 was almost always "dirty" the instant its turn came up, so it kept sending `T` and never sent the `P` that solicits an `R` — confirmed independently by the dongle, a second sniffer, the Node's own TXEN LED, and an `onTrace` packet-level capture (zero `P` to UA 30 across the whole capture window). Node 31 was not the cause in itself; it only happened to tune the round-trip period into resonance with the bitwalk. Any legitimately slow/offline node combined with any output-update cadence near the round-trip period could trigger the same starvation in the field.

Fix (library, not the example sketch): `CMRIHost` now bounds how long a due poll can be deferred by a pending transmit (`CMRIHostConfig::maxOutputPreemptMs`, default 250 ms) — once a node's last real poll is that old, the scheduler forces the poll through regardless of `outputsDirty_`. This bound only holds if the round-robin's healthy-node cycle time stays well under that threshold, so a companion poll-retry backoff (`initialPollBackoffMs`/`maxPollBackoffMs`, doubling per consecutive miss up to 32 s, cleared immediately on any accepted reply) keeps a chronically offline node from taxing every round-robin pass at full priority. Without the backoff, the anti-starvation bound alone inverts into the mirror-image defect — transmit starvation — once the round-robin's baseline cycle time is itself pinned near the bound by an unrelated offline node; both changes ship together. See `src/CMRIHost.cpp`/`.h` and `src/RemoteNodeHandle.h`, and the regression tests in `tests/test_host.cpp` (`test_dirty_output_cannot_starve_poll_forever`, `test_anti_starvation_does_not_starve_transmit`, `test_poll_backoff_doubles_and_clears_on_reply`).

Hardware re-verification (2026-08-19, same bench, unmodified `SimpleHost.ino`, fixed library): `extras/bench/flash_and_probe.sh SimpleHost` now reports `OVERALL: HEALTHY` with hundreds of `R` frames decoded per 15 s window by both the second sniffer and the dongle (A/B/A confirmed across three separate runs; `XiaoHostTracer` re-checked healthy in between). `extras/bench/verdict.py` no longer special-cases SimpleHost as an expected-silent case — a silent reply pair under any sketch is now a regression.
