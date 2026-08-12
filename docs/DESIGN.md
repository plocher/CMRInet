# CMRInet — Architecture and Design Decisions

Status: agreed baseline from design review, 2026-08-12.
This document supersedes the architecture portions of `PLAN.md`
("Why a separate library", "Protocol notes", "Core state machine").
The bench-instrument goal, display semantics, and phasing in `PLAN.md`
still stand, reinterpreted through the layer model below.

Companion documents:
- `docs/cmrinet-interop-profile-and-errata.md` — the wire rules this
  library implements, with evidence citations.
- `docs/research/` — eight adversarial implementation reviews and the
  cross-review synthesis (`comparison.md`).

## Terms

- **Host** and **Node**: the spec's names for master and slave
  (LCS-9.10.1). This project uses no other names for these roles.
- **Image**: a byte buffer of I/O bits. OB = outputs (Host intent),
  IB = inputs (Host belief). Images plus freshness are what sketches
  touch.
- **Packet**: the protocol's logical datagram `{UA, MT, body}`.
  I/P/T/R are packet types. A packet is not bytes on a wire.
- **Framing**: the serial rendering of a packet
  (SYN/0xFF SYN/0xFF STX/0x02 ... ETX/0x03, DLE/0x10 escaping).
- **Strategy**: an engine that implements the image contract by some
  exchange discipline. Polled CMRInet is the first strategy.
- **Transport**: carries packets for the polled strategy.

## One product, two seams

The product is a layout I/O image service. CMRInet is its first
strategy, not its identity. LCS-9.10.1 governs the strategy, not the
product.

```
Image contract (handles: bits, freshness, health)   <- THE product surface
        | implemented by (setup-time choice)
  PolledCMRIHost                     [MQTTCMRIHost — future, no consumer]
        |                                  |
  packet seam (CMRITransport)         MQTT client, topics, LWT
        |                                  (no packet seam at all)
  SerialCMRI / Mock / TCP / MQTT-as-carrier
```

- The **image seam** is universal. Every strategy implements it. The
  sketch selects the strategy in `setup()` and uses image verbs in
  `loop()`.
- The **packet seam** is NOT product-wide. It is the polled strategy's
  carrier boundary. It exists so CMRInet-the-protocol can ride serial,
  TCP, mock, or MQTT-as-carrier. A native push strategy has no packet
  seam — its lower edge is its own client library.

Choosing a strategy selects a compatibility domain: `PolledCMRIHost`
keeps you in the CMRInet interop world (JMRI, fielded Nodes,
gateways). A native push strategy leaves it, with the same sketch.

Litmus test for a new medium: need interop with things that speak
CMRInet → packet seam. The medium is the reason to abandon polling →
image seam (a new strategy).

### Units ladder

```
bits    — handle verbs (setOutputBit / inputBit)          loop()
images  — strategy state (OB intent, IB belief + age)
packets — polled strategy currency: T/R wrap images;
          P is media-access control (no image);
          I is session setup (no image)
bytes   — serial rendering (codec, TXEN, SYN preamble)
```

P and I are why the polled strategy's lower currency must be packets,
not images: they carry no image at all. On media with native delivery
and sessions they are vestigial but harmless.

### Fielded precedent: JMRI decomposes on the same seams

JMRI's NamedBean layer (Sensor/Turnout/Light) is its strategy-neutral
image contract at bit granularity. Its CMRI traffic controller is a
polled strategy with two carriers (serial dongle, raw TCP via
`networkdriver`). Its `jmrix.mqtt` package is a native push strategy
(beans over topics, no packets, no polling). Cross-strategy mirroring
happens at the bean layer (Routes, LogixNG, scripts). JMRI never
built CMRI-over-MQTT-as-carrier in two decades — evidence that at
broker distances you bridge images, not packets. Where our contract
improves on JMRI's: beans have no freshness or staleness concept;
our handles do.

## Decisions

### D1. Names follow the spec: Host and Node
Classes and docs use Host/Node, never master/slave or
controller/client.

### D2. The sketch-facing contract is images plus freshness and health
The per-node handle exposes output image writes, input image reads,
input age, link state, and stats. It contains no poll vocabulary.
Link states are strategy-neutral: UNINITIALIZED / ONLINE / STALE /
OFFLINE. Stats split into a generic core (exchanges, errors,
turnaround, staleness) and a strategy extension (polls, misses,
re-inits). Handle, image, freshness, and health types live in a
strategy-neutral header owned by no engine.

Two different timeouts, kept apart:
- **Reply-gate timeout** (~250 ms): how long the polled strategy holds
  the bus waiting for R. A shared-bus denial-of-service defense.
  Lives in `PolledConfig`. Meaningless to other strategies.
- **Staleness threshold**: how old input data may be before the
  application should distrust it. Universal. Lives in the handle
  contract.

### D3. One strategy, two roles, three counterparty fidelities
Build the polled strategy only, but both roles of it:
- **PolledHost** — initiates: schedule, reply-gate timeout, re-init
  ladder.
- **PolledNode** — reacts: UA match, I/T/P handling, R reply. It is
  the test counterparty, the emulator core, and the eventual client
  library candidate. Building the Host's test rig builds it anyway.

Counterparty fidelity ladder, in build order:
1. **Scripted replay** at the byte/packet level, for unit tests.
   Deliberately dumber than an emulator so pathological sequences
   from the research replay exactly.
2. **Correct PolledNode**, compliant with the interop profile, for
   loopback integration tests and default emulated nodes.
3. **Warty PolledNode**: configurable fielded defects (Adams 50 ms
   reply delay, third-SYN frame drop, DLE-blind flush, body shift,
   trailing I-body byte). The research reviews are its requirements.

No abstract strategy base class yet: one concrete strategy, and the
shared handle types carry the contract. No second strategy until it
has a real consumer.

### D4. Transports carry packets; the codec lives in the serial adapter
The polled engine's lower edge is packet-in / packet-out. Framing
exists only to create message boundaries on a byte stream, so it
belongs to the serial transport. Byte-level concerns live in the
serial adapter: TXEN discipline (assert, write, flush to drain,
deassert), inter-byte timeout, dangling-DLE handling, gapless
single-write emission, stop-bit config, UART error counters, two-SYN
emission. Protocol-level concerns stay in the strategy: poll schedule,
reply-gate timeout, re-init ladder, I-then-full-T sequencing, dH/dL
policy, UA/MT reply verification, health.

### D5. Two-phase lifecycle; allocation only at setup
Configuration phase: `addNode()` and handler registration; allocation
is legal (embedded practice: allocate at startup, never free).
`begin()` locks the configuration. After `begin()`: no allocation and
no deallocation, ever. Nodes cannot be removed, only disabled.
Applies to transports too.

### D6. Non-blocking tick with injected time
The engine advances only from `tick(nowMs)`. Nothing in the library
blocks, sleeps, or busy-waits. A zero-argument `tick()` convenience
uses `millis()`. Injected time plus injected transport lets the whole
engine run against a mock clock and mock transport in desktop unit
tests, deterministically and faster than real time.

### D7. Embedded-first C++ subset at the API surface
Function pointers or template functors, not `std::function`. Error
codes, not exceptions. No RTTI, no `String`, no growable containers
after `begin()`. Received bytes normalize to `uint8_t` at the read.
Observability is listener registration (JMRI pattern): metrics,
monitor, and trace hooks are optional listeners the linker drops when
unused. No feature `#ifdef`s inside the library.

### D8. Floor: ESP32-class drives the design; AVR gets a mini profile
The full bench instrument targets ESP32-class parts. Geometry
ceilings (max nodes, max body bytes) are compile-time knobs, so a
'328-class build supports a single-node cpNode+IOX diagnostic tester
within its limits. AVR-as-Host is supported within limits, not a
design driver.

### D9. Policy defaults come from the research
Defaults match what JMRI-tuned Nodes expect, per-node overridable:
250 ms reply-gate timeout; more than 5 consecutive misses triggers
the re-init ladder (I, then immediately a full T, then invalidate
cached inputs); ~500 ms post-I settle; ~2 ms post-T gap; ~5 ms poll
pacing; dH/dL = 0. T is sent on change with the full image, plus
optional periodic refresh (off by default). Output-only Nodes get a
keepalive poll. Silent Nodes are polled forever and surfaced as
OFFLINE, never dropped. A "classic reference mode" (full-image T
every cycle, generous timeout, never re-init, 8N2) is a planned
emulation option for A/B work.

### D10. Wire behavior follows the interop profile
Framing, escaping (all bodies including I), SYN policy, stop bits,
UA verification, buffer sizing (256-byte bodies, 2x TX staging), and
recovery rules implement `docs/cmrinet-interop-profile-and-errata.md`
Part 2. Where the profile and the spec text disagree, the profile
wins, and the erratum (Part 1) records why.

### D11. The MQTT carrier transport is the third-party seam proof
A `MqttCMRITransport` implemented by an independent party, against
the transport contract alone, validates the packet seam. Acceptance:
the desktop loopback rig (PolledHost to PolledNode) runs over it with
zero engine changes and all mock-transport suites still passing.
Dispatch only after the codec, engine, and mock transport exist.
Speed is explicitly not a goal. It is a proof, not a product — see
D12 for the product-shaped MQTT story.

### D12. The semantic gateway is the product's first application
An ESP32 gateway device runs `PolledCMRIHost` at the RS-485 segment
(polling at wire speed) plus a thin topic-mirror app written against
the image verbs: publish IB images and per-node state on change
(retained), subscribe to OB image topics, gateway LWT for liveness.
No new architecture — product plus mirror sketch.

Host-location litmus for any remote scenario:
- Host at the gateway → images cross the medium (this design).
- Host remote and already exists (for example JMRI must stay the
  Host) → packets cross the medium (the JMRI-over-TCP pattern; the
  MQTT variant is D11).

Freshness must cross the mirror explicitly: retained messages age
invisibly, so per-node state/age topics and the gateway LWT are part
of the schema, not optional extras.

Interop opportunity: align the mirror's topic and payload schema with
JMRI's `jmrix.mqtt` conventions (`MqttContentParser`), so any JMRI
instance consumes the gateway's segment as ordinary MQTT
sensors/turnouts with no CMRI connection. JMRI then doubles as a
third-party integration test for the image schema, the same role the
MQTT transport plays for the packet seam.

## Transport contract (packet seam)

Scope: transports for the CMRInet polled strategy. Not a product-wide
layer (see D4, one-product model).

```cpp
class CMRITransport {
 public:
  virtual void begin() = 0;                        // may allocate (D5)
  virtual void tick(uint32_t nowMs) = 0;           // sole CPU entry; never blocks
  virtual bool sendPacket(const CMRIPacket&) = 0;  // accepted != on the wire
  virtual bool sendComplete() const = 0;           // true once fully delivered
  virtual bool receivePacket(CMRIPacket&) = 0;     // whole validated packets only
  virtual const LinkStats& stats() const = 0;      // link errors and liveness
};
```

Clause semantics (normative for implementers):
- `sendPacket()` must not block. It returns false only for
  backpressure or link-down. The caller retries on a later tick.
- `sendComplete()` gates the strategy's reply timer. Serial: the last
  byte left the shift register and TXEN dropped. Message transports:
  the client accepted the message for delivery.
- `receivePacket()` returns packets in arrival order, at most one per
  call, and only packets that passed the transport's integrity checks
  (framing, escaping). Address filtering is NOT the transport's job.
- All client work (UART pump, MQTT keepalive, reconnect) happens
  inside `tick()`, non-blocking.
- Link liveness and error counters surface through `stats()` in
  transport-neutral terms, so the strategy folds them into node
  health without knowing the medium.
- Allocation only in `begin()` (D5).

Known implementer decisions for a message-carrier transport (MQTT):
1. Topic scheme: per-UA topics are legal and preferred. The engine
   never assumes a shared medium.
2. Payload: naked `{UA, MT, body}` versus fully framed bytes. Framed
   payloads make a serial bridge a dumb byte pump. Choose and justify.
3. QoS/retain mapping: QoS 0 mirrors the wire. A retained output
   image gives late joiners the last T. Document the choice.

## Handle contract (strawman, pre-implementation)

```cpp
CMRIHost host(transport);                       // config phase
CMRINodeHandle& n = host.addNode(ua, NodeConfig{...});
host.onEvent(fn);                               // timeouts, re-inits, errors
host.onTrace(fn);                               // TX/RX packets (monitor)
host.begin();                                   // lock; no allocation after

host.tick(nowMs);                               // runtime, non-blocking

n.setOutputBit(bit, v);                         // marks dirty -> full-image T
n.setOutputs(image, len);
bool b       = n.inputBit(bit);                 // last good IB
uint32_t age = n.inputAgeMs(nowMs);             // staleness (D2)
LinkState s  = n.state();                       // UNINITIALIZED/ONLINE/STALE/OFFLINE
const NodeStats& st = n.stats();
n.setEnabled(false);                            // no removal (D5)
```

Open items to settle during tracer-bullet implementation:
1. Default output semantics: T-on-change (JMRI) per D9. Confirm on
   the bench. `forceTransmit()` exists either way.
2. Per-node input-change callback, or polled-only handle. Start
   polled-only. Add the callback only if diff-scanning hurts.
3. Final name for the per-node handle (`CMRINodeHandle` here) so it
   does not collide with the cpNode library's mental namespace.

## Test strategy

- Unit: codec against byte vectors, including every pathological case
  in `comparison.md` §3 (the anti-checklist is the test plan). Engine
  against mock clock plus scripted-replay transport (fidelity 1).
- Integration: desktop loopback, PolledHost to PolledNode over paired
  mock transports (fidelity 2), later over `MqttCMRITransport` (D11).
- Conformance/bench: warty PolledNode (fidelity 3), fault injection,
  slow byte-spaced TX (classic Hosts sent gapped bytes; see profile
  2.2.6), oversized frames, truncations.
- Hardware: the PLAN.md two-board bench (Xiao Host plus Xiao_I2C
  node) validates TXEN timing and real-wire behavior mocks cannot.

## Scope for the tracer bullet (Phase 1, revised)

In: codec, serial transport, mock transport, PolledHost (P/R only),
minimal handle (inputs, freshness, state), scripted-replay tests,
OLED hit/miss display per PLAN.md.
Out (sequenced, not abandoned): T and I sending (Phase 2), PolledNode
emulator (Phase 2, as the loopback counterparty), MQTT carrier proof
(after Phase 2), semantic gateway app (after the emulator), push
strategy (no consumer), SUSIC/SMINI node types (bench roadmap).
