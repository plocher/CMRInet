# CMRInet testbed physical notes
These notes capture the physical side of the bench: boards, cables, adapters, and wiring. They are project-private and experimental.

Companion document: [testbed-software-notes.md](testbed-software-notes.md) covers the software substrate, use cases, and command-and-control.

## Starting inventory
Minimum bench (stage 1, desktop Host):
- one cpNode-Xiao board (Xiao ESP32-C6 + MAX3491 RS-485 transceiver + I2C + I2C OLED) — the device under test
- one USB to RS-422/full-duplex-RS-485 adapter — the Mac end of the 4-wire bus (a 2-wire A/B dongle works only with the node jumpered to 2-wire; see wiring notes)
- two RS-485 twisted pairs between adapter and board (poll pair and reply pair)
- USB-C cable to the board for flashing, power, and the CDC debug stream
- loopback jumpers on the node (outputs wired back to inputs) for self-test scenarios

A second cpNode-Xiao board unlocks:
- stage 2 (Xiao Host R&D image) — the two-board bench from PLAN.md
- scenarios that need a known-good reference node on the same bus as the device under test

A second adapter unlocks:
- a passive bus tap: listen-only, logging decoded frames with timestamps, an independent witness when Host and Node disagree
- on 4-wire, one receiver hears one pair. A full-conversation tap needs a receiver on the poll pair and another on the reply pair (one full-duplex adapter provides only one receiver, so a complete tap takes two adapters or a 2-wire bench)

## Pair naming and the crossover rule
The cpNode-Xiao RS-485 block has five positions: `T-`, `T+`, `R-`, `R+`, shield. The MAX3491 is a full-duplex part: `T±` is the board's driver pair, `R±` is its receiver pair. The labels are perspective-relative — they describe what THIS board does on that pair, not the pair's role on the bus.

Name the bus pairs by function instead:
- **poll pair** — carries Host-to-Node traffic (I, T, P)
- **reply pair** — carries Node-to-Host traffic (R)

Wiring rule: a device's `R±` terminals attach to the pair that carries traffic toward it. A Node puts `R±` on the poll pair and `T±` on the reply pair. The same board used as a Host inverts: `T±` on the poll pair, `R±` on the reply pair. The inversion is entirely in the cable — firmware, UART, and TXEN wiring are identical in both roles.

Bench consequence: the two-board Host↔Node cable is a crossover — `T+`→`R+` and `T-`→`R-` in each direction. Straight polarity, crossed function.

## Topology per stage
Stage 1 — desktop Host:
`Mac ──USB── RS422/485 adapter ──poll + reply pairs── cpNode-Xiao (loopback jumpers)`
Adapter TX pair → poll pair, adapter RX pair → reply pair. The Mac also holds the node's USB-C for flash, power, and telemetry. Two cables to the bench.

Stage 2 — Xiao Host:
`Mac ──USB── Xiao Host ──crossover (poll + reply pairs)── cpNode-Xiao node`
Both boards on Mac USB for flash, power, and their CDC streams. The Mac's adapter can stay attached as a passive tap, or as an alternate Host.

Hazard: two Hosts on one bus. Only one may drive the poll pair. The `quiesce`/`resume` verbs (software notes) exist for this handoff. Until they exist, physically disconnect the adapter that must stay silent.

## Wiring and electrical notes
- The protocol is half duplex (strict poll/response) even though the 4-wire physical layer could carry both directions at once. TXEN discipline on the reply pair is the classic failure point — the bench exists partly to catch it on a real wire.
- 4-wire echo visibility: a Node's receiver sits only on the poll pair, so it never hears its own reply or other nodes' replies. The Host never hears its own polls. Code validated only on 4-wire has never seen reply traffic or self-echo on RX.
- Deployment norm: most historical CMRI installs, and as far as we know all current ones, are 4-wire. The 4-wire bench is fidelity to the field. 2-wire is an adversarial variant only, not a deployment target.
- 2-wire conversion: tie `R+` to `T+` and `R-` to `T-` at the terminal block (legal with the MAX3491; the classic Chubb 2-wire conversion). One pair, one cheap A/B dongle — but now every receiver hears everything, including its own echo. That is the harsher RX environment. Given the deployment norm, it is a low-priority conformance scenario, not a bench default.
- Host TXEN choice: on 4-wire the Host is the only driver on the poll pair and could hold DE asserted permanently, as classic CMRI hosts did. Prefer identical TXEN discipline in both roles: it exercises the shipped code path and survives a later 2-wire jumpering.
- Termination and biasing: the board has an on-board termination option. A short bench pair usually works unterminated, but decide deliberately and record what the bench uses, so timing anomalies are not chased into the wrong layer.
- Loopback jumpers invert: outputs are active low, so out 0 reads back as in 1. Scenario assertions must apply the inversion.
- **Loopback safety rule (hardware-protecting, non-negotiable): a pin pair joined by a loopback jumper must never have both ends configured as outputs.** Configure exactly one end as OUTPUT and the other as INPUT — or both as INPUTs. Two push-pull drivers fighting through a jumper wire is a hardware-damage risk, and nothing in the protocol or the expander prevents it. Every sketch flashed to a jumpered node must be defensively configured against its board's jumper map before upload.
- Bench IOX32 jumper inventory (MRCS IOX32 boards on the stage-1 node, recorded at build time — issue #22):
  - `0x20` — no loopback jumpers
  - `0x21` — two jumpers
  - `0x22` — all 8 port A pins jumpered to port B
  - `0x23`, `0x24` — no loopback jumpers
  Scenario guidance: use the unjumpered boards for pure input reads, `0x21`/`0x22` for loopback assertions once T lands (Phase 2). A scenario selects boards by I2C address; the sketch's direction map must honor the safety rule above for whichever boards it enables.
- Power: both boards can run from Mac USB during bench work. Note any externally powered configuration in the scenario, since brownout during pattern bursts would masquerade as protocol faults.
- Manual production-test wiring (card N outputs to card M inputs) is operator work, guided step by step by the runner. The bench does not attempt relay matrices or automated patch panels.

## Known physical gaps
- Agent-controlled power cycling. Negative tests (unplug the node, watch NO-RESPONSE and recovery) stay human-in-the-loop. A per-port switchable USB hub (uhubctl-compatible) would close this later.
- USB port identity. Multiple Xiaos enumerate as shuffling /dev/cu.usbmodem* names. Deferred with the port registry (software notes); until then, plug in one board at a time or check serial numbers with `arduino-cli board list`.
- TXEN edge timing proof. The passive tap catches most disputes; a logic analyzer is the deluxe option if turnaround timing itself is ever in question.

## Open setup assumptions (to challenge)
- One bus segment, short pair, bench distances only. Nothing here addresses layout-scale wiring runs.
- All power over USB from the Mac. No isolated supplies, no ground-loop consideration between adapter and boards.
- The Mac is the only computer. No Pi or Mac mini bench host until an always-on need is real.
- Flashing over USB only. WiFi OTA by hostname exists in the node sketch but is a human convenience, not the agent path, during the tracer effort.
- Bench is set up per-session. Nothing assumes an always-connected fixture yet.
