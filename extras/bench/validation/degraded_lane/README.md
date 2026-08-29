# Degraded-lane bounding validation (#87 bench acceptance, #88 done-when 4)

On-hardware proof that the degraded-lane allocator (#87, D17) bounds the
poll share of ALL imperfect nodes — the condition #80 recorded and the
unit tests refute against a mock transport.

## Purpose

#80 found a node declared NI=4 answering with 3 bytes took 1316 polls to
a healthy node's 765 — serviced 1.7x MORE than the node doing real work,
while committing nothing. #87 added two gates (Gate A: slot share, Gate
B: wall-clock bandwidth) and a conformance breaker to bound it. The unit
tests (`test_degraded_participation_is_bounded`,
`test_gates_alone_bound_degraded_share`,
`test_degraded_class_is_never_starved_to_zero`) prove the logic against a
mock transport with a three-node population. This suite proves it on the
real RS485 bench with the same three-node shape.

## Scenario

One scenario: `degraded_lane_bounding`. Three nodes, matching the unit
test's population:

- **UA30** (healthy): compiled in at 7/7, conforming, with slowwalker
  generator load so the round-robin has real contention.
- **UA31** (degraded, misconfigured): a real alive node declared 4/4
  against its physical 3/3. Its replies carry the wrong geometry and
  are rejected, so it enters the degraded lane without committing data.
  This is the #80 condition.
- **UA32** (degraded, silent): the compiled-in phantom — a nonexistent
  node that never replies. Its polls are valid degraded-lane traffic,
  not pollution: the 80/20 rule bounds ALL imperfect nodes, and a silent
  node is the other failure mode the gates exist to bound (Gate B:
  wall-clock bandwidth — a silent probe burns a full reply timeout).

## What the analyzer scores

| Question | Seam | Evidence |
|----------|------|----------|
| Poll distribution (who got polled how often) | trace (ring dump TX P) | per-UA TX P count |
| Did a gate actually bind? | semantic (host status) | degradedGrants, degradedSlotDenials, degradedBandwidthDenials |
| Did the healthy node commit data? | semantic (per-node status) | accepted exchanges, state |
| Did each degraded node commit nothing? | semantic (per-node status) | accepted exchanges = 0 |

The degraded share is the SUM of all degraded nodes' polls (UA31 +
UA32), matching the unit test's `degradedPolls = nodes[1].polls +
nodes[2].polls`.

**Pass** when:
- degraded polls (sum) < healthy polls (the #80 inversion is gone)
- degraded share (sum) < 35% (bounded, matching the unit test)
- ledger has grants > 0 AND denials > 0 (a gate bound)
- healthy node online with accepted exchanges ≥ 10
- each degraded node has 0 accepted exchanges

## Run

```shell
extras/bench/.venv/bin/python extras/bench/validation/degraded_lane/gather_degraded_lane.py
```

Then analyze:

```shell
extras/bench/.venv/bin/python extras/bench/validation/degraded_lane/analyze_degraded_lane.py extras/bench/validation/degraded_lane/data/results.YYYYMMDD.degraded_lane
```

## Bench topology note

The bench is currently 2-wire (single-pair), so the host's CDC ring dump
sees all TX (and the self-echo on RX, which echo-cancel discards). Both
sniffers would see the same 100% of traffic, so sniffer witnesses are
optional here — the host's own trace is authoritative for poll
distribution. This gather follows the CDC-only pattern used by the
dual_node and host_view suites.

## Physical node geometry

Per `docs/testbed-physical-notes.md`: UA31 is a real cpNode with 3 input
bytes and 3 output bytes. Declaring it 4/4 to the Host reproduces the
#80 mismatch: the Host polls, the node replies with 3 bytes, the Host
rejects the geometry mismatch, and the node enters the degraded lane
without committing data. UA32 is the compiled-in phantom (use it if you
need a nonexistent node).
