# CMRInet

A protocol suite for **CMRInet** (NMRA LCS-9.10.1): **Host** and
**Node** engines over a shared codec and pluggable transports, plus a
bench-instrument emulator. The design frames the product as a layout
I/O image service — sketches deal in I/O images, freshness, and node
health; CMRInet polling is the first exchange strategy beneath that
contract. See [docs/DESIGN.md](docs/DESIGN.md) for the layer model and
decisions D1-D12.

Status: **design baseline agreed; tracer bullet next** — see
[PLAN.md](PLAN.md) (phasing) and [docs/DESIGN.md](docs/DESIGN.md)
(revised Phase 1 scope). Agents picking up this work: start with
[HANDOFF.md](HANDOFF.md).

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

## Planned layout

- `src/` — codec, transports (serial/RS-485, mock), PolledHost and
  PolledNode engines, strategy-neutral handle types.
- `examples/` — bench "JMRI Master" instrument (Xiao ESP32-C6, OLED,
  WiFi OTA), node emulator, MQTT semantic gateway.

Related libraries in this family: [`cpNode`](../cpNode) (deployed
Node-side library), [`cpCMRI`](../cpCMRI) (enhanced Node-side
library). This suite deliberately builds fresh rather than refactoring
deployed code; a merge discussion is deferred by design (PLAN.md
Phase 3, DESIGN.md D3).
