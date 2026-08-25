# CMRInet

A protocol suite for **CMRInet** (NMRA LCS-9.10.1): **Host** and
**Node** engines over a shared codec and pluggable transports, plus a
bench-instrument emulator. The design frames the product as a layout
I/O image service — sketches deal in I/O images, freshness, and node
health; CMRInet polling is the first exchange strategy beneath that
contract. See [docs/DESIGN.md](docs/DESIGN.md) for the layer model and
decisions D1-D17.

Status: **Host engine, bench tracer, and sniffer are built; the
front-door Host example (`examples/SimpleHost`) lands with this
change.** The Node-side engine (`CMRINode`) is issue #9. See
[PLAN.md](PLAN.md) for phasing and [HANDOFF.md](HANDOFF.md) for the
bench environment. Agents picking up this work: start with
[HANDOFF.md](HANDOFF.md).

## Architecture at a glance

CMRInet is a layout I/O image service. A sketch deals in images,
freshness, and node health. It does not deal in packets or bytes.

One product, two seams:

- **The image seam** is the product surface. Every strategy implements
  it. A sketch selects a strategy in `setup()` and uses image verbs in
  `loop()`.
- **The packet seam** is the polled strategy's carrier boundary. It
  exists so CMRInet can ride serial, TCP, mock, or MQTT-as-carrier. A
  native push strategy has no packet seam.

Units ladder, from the sketch down to the wire:

- **bits** — handle verbs (`setOutputBit`, `inputBit`)
- **images** — strategy state: output intent and input belief plus age
- **packets** — polled strategy currency: T and R wrap images; P is
  media access; I is session setup
- **bytes** — serial rendering: codec, TXEN, SYN preamble

A sketch holds a `RemoteNodeHandle` per remote node. The handle exposes
output writes, input reads, input age, health state, and statistics. It
carries no poll vocabulary. `RemoteNodeState` is strategy-neutral and
derived from stored axes (liveness, image state, conformance):
`UNINITIALIZED`, `ONLINE`, `STALE`, `OFFLINE`, `MISCONFIGURED`,
`DEGRADED`.

Names follow the spec: **Host** and **Node**, never master or slave.
The naming grammar puts the head noun last. Qualifiers stack in front.
Innermost is what a thing speaks. Outermost is what it runs on. All
public types live in `namespace CMRInet`.

The full layer model, the seam contracts, and the reasoning behind each
decision are in [docs/DESIGN.md](docs/DESIGN.md) (D1-D17). This section
is a map. DESIGN is the spec.

## Getting started

Open `examples/SimpleHost/SimpleHost.ino`. It is the front-door example
for the Host side. The sketch polls remote nodes, shows each node's
health on an OLED, and runs a small behavior: a blinking output and an
output that toggles on an input edge.

To run it you need:

- A cpNode-Xiao board (Seeed XIAO ESP32-C6 + MAX3491) as the Host.
- One or more cpNode-family nodes on the same RS-485 block.
- The Adafruit SSD1306 and GFX libraries for the OLED. Set `USE_OLED`
  to 0 to compile the display out.

Wire the Host's T± to the Node's R± and the Host's R± to the Node's
T±. Edit the `nodeTable` array in the sketch to match your nodes'
addresses and I/O byte counts. Upload and open a serial monitor.

The other Host examples are bench instruments. `XiaoHostTracer` is a
command-driven R&D tracer. `XiaoSniffer` is a passive bus tap. Use the
tracer to poke a node interactively. Use SimpleHost as the starting
point for a layout.

The Node-side engine is issue #9. This README covers the Host side now.
It will extend to cover both roles when #9 lands.

## Documents

- [docs/DESIGN.md](docs/DESIGN.md) — architecture: one-product layer
  model, image/packet seams, transport contract, handle contract.
- [docs/cmrinet-interop-profile-and-errata.md](docs/cmrinet-interop-profile-and-errata.md)
  — "CMRInet as fielded": normative interop rules and proposed
  LCS-9.10.1 errata, evidence-cited.
- [docs/research/](docs/research/) — eight adversarial reviews of
  fielded implementations (four Node-side, three Host-side) against
  the spec, plus the cross-review synthesis
  ([comparison.md](docs/research/comparison.md)).
- [docs/lcs-9.10.1_cmrinet_v1.1.pdf](docs/lcs-9.10.1_cmrinet_v1.1.pdf)
  — the NMRA specification.

## Repository layout

- `src/` — codec, transports (serial/RS-485, mock), the polled Host
  engine, strategy-neutral handle types, and the shared testbed engine.
- `examples/SimpleHost` — the front-door Host tutorial (Xiao ESP32-C6,
  OLED, behavior-only).
- `examples/XiaoHostTracer` — the bench R&D tracer (Xiao ESP32-C6, USB
  CDC command stream, JSON-lines telemetry).
- `examples/XiaoSniffer` — a passive RS-485 bus tap (Xiao ESP32-C6,
  OLED, JSON-lines frame log).
- `tests/` — desktop unit tests (145 tests, no Arduino dependencies).
- `extras/desktop/` — the desktop tracer binary.

Related libraries in this family: [`cpNode`](../cpNode) (deployed
Node-side library), [`cpCMRI`](../cpCMRI) (enhanced Node-side
library). This suite deliberately builds fresh rather than refactoring
deployed code; a merge discussion is deferred by design (PLAN.md
Phase 3, DESIGN.md D3).
