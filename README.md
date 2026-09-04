# CMRInet

A protocol suite for **CMRInet** (NMRA LCS-9.10.1): **Host** and
**Node** engines over a shared codec and pluggable transports, plus a
bench-instrument emulator. The design frames the product as a layout
I/O image service — sketches deal in I/O images, freshness, and node
health; CMRInet polling is the first exchange strategy beneath that
contract. See [docs/DESIGN.md](docs/DESIGN.md) for the layer model and
decisions D1-D17.

Status: **Host and Node engines are built, with desktop loopback
integration tests passing.** The front-door examples are `SimpleHost`
and `SimpleNode`. Bench instruments (`TracerHost`, `XiaoSniffer`)
are built for the testbed. See [docs/DESIGN.md](docs/DESIGN.md) for
the architecture and [docs/adr/0004-library-boundary-and-transport-packaging.md](docs/adr/0004-library-boundary-and-transport-packaging.md)
for the library boundary decision.

## Architecture at a glance

This library is the CMRI strategy. A sketch deals in images,
freshness, and node health. It does not deal in packets or bytes.

- **The image seam** is this library's top edge. The sketch uses
  image verbs in `loop()`.
- **The packet seam** is the polled strategy's carrier boundary. It
  exists so CMRInet can ride serial, TCP, or mock. Second strategies
  (push, rich semantics) live in a sibling library (ADR-0004).

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

The other Host examples are bench instruments. `TracerHost` is a
command-driven R&D tracer. `XiaoSniffer` is a passive bus tap. Use the
tracer to poke a node interactively. Use SimpleHost as the starting
point for a layout.

### Node side

Open `examples/SimpleNode/SimpleNode.ino`. It is the front-door example
for the Node side — the simplest way to make a device act as a CMRInet
node. The sketch reads a pushbutton into an input bit and drives an LED
from an output bit, using the direct accessors in `loop()`.

For real layout I/O on I2C expanders, see `examples/XiaoNode/XiaoNode.ino`
— the full-featured Node with OLED status, WiFi OTA, and the
`onPack`/`onUnpack` callback pattern for MCP23017 expander I/O. Toggle
`USE_OLED` / `USE_OTA` at the top of the sketch; put WiFi credentials in
`secrets.h` (see `secrets.h.example`), not in the committed sources.

`examples/TracerNode` is the bench C&C test mule for internal validation
against the Host tracer.

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

- `src/` — codec, the polled Host and Node engines, strategy-neutral
  handle types, and the shared testbed shell.
- `src/transport/` — transport implementations: `mock.h` (test double),
  `serial.h` (RS-485), `serialESP32.h` (hardware TX-drain port), and
  the byte-port seam (`serialPort.h`, `serialStream.h`). The umbrella
  carries the seam (`CMRITransport.h`); a sketch includes the
  implementation it chooses.
- `examples/SimpleHost` — the front-door Host tutorial (Xiao ESP32-C6,
  OLED with segmented flush, miss/error health rows, behavior-only).
- `examples/SimpleNode` — the front-door Node tutorial (Xiao ESP32-C6,
  pack/unpack seam, minimal GPIO).
- `examples/TracerHost` — the bench R&D tracer (Xiao ESP32-C6, USB
  CDC command stream, JSON-lines telemetry).
- `examples/TracerNode` — the bench Node test mule (capture, trace, C&C).
- `examples/XiaoNode` — the full-featured Node (OLED, WiFi OTA, I2C
  expanders via pack/unpack).
- `examples/XiaoSniffer` — a passive RS-485 bus tap (Xiao ESP32-C6,
  OLED, JSON-lines frame log).
- `tests/` — desktop unit tests (290 tests, no Arduino dependencies).
- `extras/bench/` — RS-485 bus probe scripts and single-use bench
  jigs (`XiaoBenchCal`, `XiaoBenchEcho`, `XiaoBenchEchoCancel`).
- `extras/desktop/` — the desktop tracer binary.

Related libraries in this family: [`cpNode`](../cpNode) (deployed
Node-side library), [`cpCMRI`](../cpCMRI) (enhanced Node-side
library). This suite deliberately builds fresh rather than refactoring
deployed code; a merge discussion is deferred by design (PLAN.md
Phase 3, DESIGN.md D3).
