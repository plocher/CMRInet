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

`extras/bench/bench list`:
| Type    | ID    | Serial            | Location | Device                     | Status |
| ------- | ----- | ----------------- | -------- | -------------------------- | ------ |
| Host    | Host  | 58:E6:C5:1A:88:D8 | 4-1.1    | /dev/cu.usbmodem41101      | OK     |
| Sniffer | RX    | A0:F2:62:86:B8:18 | 4-1.4.2  | /dev/cu.usbmodem414201     | OK     |
| Sniffer | TX    | 54:32:04:21:7E:54 | 4-1.4.1  | /dev/cu.usbmodem414101     | OK     |
| Node    | 30    | 58:E6:C5:1A:77:74 | 4-1.2    | /dev/cu.usbmodem41201      | OK     |
| Node    | 31    | A0:F2:62:85:CC:64 | 4-1.4.3  | /dev/cu.usbmodem414301     | OK     |
| Dongle  | RS485 | BG04ID4L          | 5-1      | /dev/cu.usbserial-BG04ID4L | OK     |

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
- Bench I/O expander jumper inventory 
  - UA31 (NI=3, NO=3): 
    - bytes 0,1 phantom (not implemented)
    - byte 2 IN bit 1 ↔ OUT byte 2 bit 1
    - byte 2 IN bit 2 ↔ OUT byte 2 bit 2
    - Walker on byte 2
  - UA30 (NI=6, NO=6): 
    - bytes 0,1 phantom. 
    - byte 2 IN and OUT available
    - byte 3 IN bit 1 ↔ OUT byte 2 bit 1
    - byte 3 IN bit 2 ↔ OUT byte 2 bit 2
    - byte 4 IN all 8 bits  ↔ OUT byte 4
    - byte 5 OUT bit 1 → byte 6 IN bit 1 (cross-byte). 
    - Walker on byte 3

  Scenario guidance: use the unjumpered chips (`0x20`, `0x23`, `0x24`)
  for pure input reads, `0x21`/`0x22` for loopback assertions once T
  lands (Phase 2). A scenario selects chips by I2C address; the
  sketch's direction map must honor the safety rule above for
  whichever chips it enables.
- Power: all boards can run from Mac USB during bench work. Note any externally powered configuration in the scenario, since brownout during pattern bursts would masquerade as protocol faults.
- Manual production-test wiring (card N outputs to card M inputs) is operator work, guided step by step by the runner. The bench does not attempt relay matrices or automated patch panels.
## Wire-visible signatures for XiaoHostTracer stimulus generators
When `XiaoHostTracer` (v0.3.0+) drives a stimulus generator, the T-frame payload has a specific bit pattern. This lets a bench observer confirm the generator is correct without relying only on CDC verb responses. The first #55 implementation inverted the `fastwalker` logic. Wire inspection caught the problem; unit tests did not.
- **`fastwalker`** (default byte 3, 250 ms period): walks a cleared bit through a field of set bits. Byte 3 progresses `0xFF → 0xFE → 0xFD → 0xFB → 0xF7 → 0xEF → 0xDF → 0xBF → 0x7F → 0xFF …`.
- **`slowwalker`** (default byte 5, 1000 ms period): walks a set bit through a field of cleared bits. Byte 5 progresses `0x00 → 0x01 → 0x02 → 0x04 → 0x08 → 0x10 → 0x20 → 0x40 → 0x80 → 0x00 …`.
- **`toggleoutfrominput`** (default input bit 48 → output bit 32): inverts output bit 32 on the rising edge of input bit 48. On the wire, this is a single-bit toggle in byte 4, bit 0 of the T payload.
- **`stall`**: has no direct T-payload signature. Its signature is a change in poll cadence, specifically the miss/backoff behavior on the phantom UA that #47 investigates.

The walkers use different output bytes by default so they can run concurrently without modifying the same byte. UA=30 uses a 7-byte output image on this bench.

## USB board identity and recovery
Multiple Xiao ESP32-C6 boards enumerate with similar and unstable `/dev/cu.usbmodem*` names. A port number is not a board identity. During #55 validation, `XiaoHostTracer` was initially uploaded to the passive Sniffer board. Commands were accepted, but the supposed Host stayed offline and emitted only miss behavior because the physical bench roles had been reversed.

Before flashing or starting an automated test:
1. Run `arduino-cli board list` to enumerate candidates.
2. Confirm the image identity over CDC, not only the port path:
   - `XiaoHostTracer` answers `status` with its tracer image/version.
   - `XiaoSniffer` emits periodic stats with `"image":"xiao_sniffer"`.
   - `SimpleHost` normally stays silent except on a rejected reply.
3. Confirm physical role: the Host board is attached to the bus crossover as the poll-pair driver; the Sniffer remains passive.
4. After any corrective flash, restore every board to its intended image before trusting test results.

Automated harnesses must match the exact expected sketch identifier and minimum version before issuing commands. Receiving any valid boot/status JSON is insufficient: the wrong image can parse commands or emit plausible telemetry while the bench topology is invalid.

## Known physical gaps
- Agent-controlled power cycling. Negative tests (unplug the node, watch NO-RESPONSE and recovery) stay human-in-the-loop. A per-port switchable USB hub (uhubctl-compatible) would close this later.
- Stable USB port identity. Port names still shuffle between enumerations. Until the port registry described in the software notes exists, identify the active image over CDC as described above; use `arduino-cli board list` only to discover candidate ports.
- TXEN edge timing proof. The passive tap catches most disputes; a logic analyzer is the deluxe option if turnaround timing itself is ever in question.

## Flashing the cpNode-Xiao from the command line
The cpNode-Xiao board is a Seeed XIAO ESP32-C6. The arduino-cli FQBN is `esp32:esp32:XIAO_ESP32C6`. The CMRInet library lives outside the default Arduino libraries directory, so compile passes it with `--library`. The Adafruit GFX, SSD1306, and BusIO libraries are required for the sketches that use the OLED (SimpleHost, XiaoSniffer).

The library path on this bench is `/Users/jplocher/Dropbox/Arduino/libraries`. Substitute the local path where the CMRInet and Adafruit libraries live.

Compile a sketch to a known build directory:
```shell
arduino-cli compile \
  --fqbn esp32:esp32:XIAO_ESP32C6 \
  --library /Users/jplocher/Dropbox/Arduino/libraries/CMRInet \
  --library /Users/jplocher/Dropbox/Arduino/libraries/Adafruit_GFX_Library \
  --library /Users/jplocher/Dropbox/Arduino/libraries/Adafruit_SSD1306 \
  --library /Users/jplocher/Dropbox/Arduino/libraries/Adafruit_BusIO \
  --build-path /tmp/<sketch>_build \
  examples/<Sketch>/<Sketch>.ino
```

`arduino-cli upload` does not take a `--library` flag. Upload the compiled binary from the build directory with `--input-dir`:
```shell
arduino-cli upload \
  -p /dev/cu.usbmodem<NNNN> \
  --fqbn esp32:esp32:XIAO_ESP32C6 \
  --input-dir /tmp/<sketch>_build \
  examples/<Sketch>/<Sketch>.ino
```

Replace `/dev/cu.usbmodem<NNNN>` with the target board port from `arduino-cli board list`. All three Xiaos enumerate the same FQBN, so identify a board by behavior, not by the port name: the sniffers emit `"image":"xiao_sniffer"` stats every 5 s, the tracer answers the `status` verb, and SimpleHost stays silent except on a reject.

## Open setup assumptions (to challenge)
- One bus segment, short pair, bench distances only. Nothing here addresses layout-scale wiring runs.
- All power over USB from the Mac. No isolated supplies, no ground-loop consideration between adapter and boards.
- The Mac is the only computer. No Pi or Mac mini bench host until an always-on need is real.
- Flashing over USB only. WiFi OTA by hostname exists in the node sketch but is a human convenience, not the agent path, during the tracer effort.
- Bench is set up per-session. Nothing assumes an always-connected fixture yet.
