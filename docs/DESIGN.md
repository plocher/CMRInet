# CMRInet — Architecture and Design Decisions

Status: agreed baseline from design review, 2026-08-12.
Version: 1.3 (bump when any decision or contract in this document
changes). A `// VALIDATION:` tag cites the version in which *that
clause* last changed, not the current document version. Tags are
therefore re-stamped per clause, as the clause changes and the
implementing code follows — never wholesale, because a tag naming a
version the code does not yet satisfy asserts something false. See
`docs/agents/validation-comments.md`.
Change log:
- v1.3 (2026-08-25): D14 refinement (issue #80). Detection and
  attribution are separated: layer says where a fault was observed,
  attribution is a verdict about what it means, and the two do not
  always collapse. Records the (Host assumption, Node assumption,
  observation) tuple as the derivation behind attribution, with an
  admission test for statically classifiable faults; establishes
  determinism as the discriminator for framing facts; and rules link
  integrity outside the conformance domain. Attribution stays
  two-valued — no "environment" or "indeterminate" value. No behaviour
  change, so only D14's own tags move to v1.3.
- v1.2 (2026-08-24): declared-vs-observed node geometry (issue #80).
  D1 gains the `RemoteHost*` perspective mirror; D2's health claim is
  superseded by D16; D5 is rewritten as a lifecycle contract with its
  allocation clause moved to D7; new D14 (geometry truth), D15 (state
  substrates), D16 (three health axes plus a projection), and D17
  (degraded service classes and the conformance breaker).
  Unlike v1.1, `Design v1.1` tags are NOT re-stamped wholesale: D2, D5,
  and D16 change substantively, so code that has not yet moved does not
  validate against v1.2. Tags are re-stamped per clause as the
  implementing code lands.
- v1.1 (2026-08-16): D7 platform-guard clarification, transport-
  contract estimated-drain footnote, and D13 inter-byte abort doctrine
  (issue #27). Existing `Design v1.0` tags re-stamped to v1.1.
- v1.0 (2026-08-12): initial baseline from design review.
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
  CMRIHost                    [future push engine — image seam, no
        |                      consumer yet; named by its schema when
        |                      it exists (the grammar permits MQTTHost)]
  packet seam (CMRITransport)      its own MQTT client, topics, LWT
        |                          (no packet seam at all)
  SerialCMRITransport / MockCMRITransport /
  TcpCMRITransport / MqttCMRITransport
```

- The **image seam** is universal. Every strategy implements it. The
  sketch selects the strategy in `setup()` and uses image verbs in
  `loop()`.
- The **packet seam** is NOT product-wide. It is the polled strategy's
  carrier boundary. It exists so CMRInet-the-protocol can ride serial,
  TCP, mock, or MQTT-as-carrier. A native push strategy has no packet
  seam — its lower edge is its own client library.

Choosing a strategy selects a compatibility domain: `CMRIHost`
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

Naming grammar: the head noun (last word) says what a thing IS;
qualifiers stack in front of it — innermost = what it speaks,
outermost = what it runs on.
- Engines are [protocol] x [role]: `CMRIHost`, `CMRINode`. "Polled"
  appears in no public name: CMRInet is inherently polled, so the
  word is redundant, and a non-polled strategy would not be CMRInet,
  so it must not carry the CMRI qualifier. A future push engine is
  named by the protocol/schema it speaks, once that exists
  (`MQTTHost` parses correctly under this grammar).
- Transport implementations end with the interface they implement:
  `SerialCMRITransport`, `MockCMRITransport`, `TcpCMRITransport`,
  `MqttCMRITransport` — read "CMRI-transport over <medium>". Never
  `SerialCMRI` or `CMRISerial` (headless shorthand).
- Product-layer types are strategy-neutral and carry no protocol
  qualifier. They are qualified by *perspective* instead: the
  `RemoteNode*` family is the Host's view of remote nodes —
  `RemoteNodeHandle`, `RemoteNodeConfig`, `RemoteNodeState`,
  `RemoteNodeStatistics` (issue #2).
- The mirror holds: the `RemoteHost*` family is the Node's view of its
  controlling Host (`RemoteHostConformance` and siblings, when #9
  lands). The "no bare `Host*` product types" rule below prohibits
  *bare* names; a perspective-qualified `RemoteHost*` is formed exactly
  as `RemoteNode*` is, and the same single-parse guarantee applies.
  Stating the mirror explicitly means it need not be re-derived (v1.2).
- `CMRI` never stacks directly onto a `Node`-family product name:
  `CMRINode` is itself a type, so any such composition
  (`CMRINodeStatistics`) parses two ways. `CMRINodeConfig` and
  `CMRINodeStatistics` are therefore reserved for the device
  engine's OWN types, where "the CMRINode's config/statistics" is
  the correct parse. There is deliberately no `RemoteNode` type —
  that absence keeps the `RemoteNode*` family single-parse.
- Never abbreviate `Statistics` to `Stats`: `State`/`Stats` is a
  one-letter trap.
- No bare `Host*` product types: the sketch's host object IS the
  strategy choice, so host-side types are engine-owned
  (`CMRIHostConfig`, nested `CMRIHost::RemoteNodePolicy`, future
  `CMRIHostStatistics`).

Packaging: all public types live in `namespace CMRInet`, spelled by
the grammar above — `CMRInet::CMRIHost`, not `CMRInet::Host`. The
redundancy is load-bearing: examples and docs always use fully
qualified names (safe in Arduino's hoisted prototypes), no global
names are exported today, and because in-namespace identifiers are
self-prefixed, exporting flat globals later via
`using CMRInet::CMRIHost;` declarations stays a non-breaking option.
A `namespace CMRI` was rejected: ArduinoCMRI defines a global
`class CMRI`.
Earlier drafts used `PolledCMRIHost`/`PolledHost`/`PolledNode`
interchangeably; those names are retired, as are `CMRINodeHandle`,
bare `Node*` product names, `LinkState`, `NodeStats`, and
`LinkStats` (issue #2).

### D2. The sketch-facing contract is images plus freshness and health
The per-node handle (`RemoteNodeHandle`) exposes output image writes,
input image reads, input age, link state, and statistics. It contains
no poll vocabulary. `RemoteNodeState` is strategy-neutral; as of v1.2
it is a *derived projection* over three stored axes rather than a
stored value, and it carries two further values — see D16.
`RemoteNodeStatistics` is a single neutral type — `exchanges`,
`noReplies`, `recoveries`, `errors`, turnaround, staleness age. It is
pure observation: monotonic, never reset, and never consulted to gate
behavior (D15). `consecutiveMisses` was documented here in error; it is
control state and belongs to the control substrate. Every exchange
discipline has attempts, failures, and recoveries, so no
strategy-extension type exists; the bench UI may *display*
"polls/misses", but the API vocabulary stays neutral. Anything truly
polled-only surfaces on engine-owned types (`CMRIHostStatistics`),
never on the handle.
Handle, image, freshness, and health types live in a
strategy-neutral header owned by no engine.

Two different timeouts, kept apart:
- **Reply-gate timeout** (~250 ms): how long the polled strategy holds
  the bus waiting for R. A shared-bus denial-of-service defense.
  Lives in `CMRIHostConfig` (host-wide default) with per-node
  overrides in `CMRIHost::RemoteNodePolicy`, never in the handle
  contract. Meaningless to other strategies.
- **Staleness threshold**: how old input data may be before the
  application should distrust it. Universal. Lives in the handle
  contract (`RemoteNodeConfig`).

### D3. One strategy, two roles, three counterparty fidelities
Build the polled strategy only, but both roles of it:
- **CMRIHost** — initiates: schedule, reply-gate timeout, re-init
  ladder.
- **CMRINode** — reacts: UA match, I/T/P handling, R reply. It is
  the test counterparty, the emulator core, and the eventual client
  library candidate. Building the Host's test rig builds it anyway.

Counterparty fidelity ladder, in build order:
1. **Scripted replay** at the byte/packet level, for unit tests.
   Deliberately dumber than an emulator so pathological sequences
   from the research replay exactly.
2. **Correct CMRINode**, compliant with the interop profile, for
   loopback integration tests and default emulated nodes.
3. **Warty CMRINode**: configurable fielded defects (Adams 50 ms
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

### D5. Lifecycle: what may change, when, and how failure is reported
v1.2 rewrite. This decision previously conflated *lifecycle* with
*storage strategy*. Allocation discipline moved to D7; capacity
ceilings remain in D8. D5 now governs the mutation contract alone.

`begin()` marks the transition from configuration to running. It is
idempotent. It does not lock the node table.

The node table is mutable at runtime within the D8 ceiling:
- **add** — legal before and after `begin()`.
- **delete** — tombstones the slot; a later add may reuse a cleaned
  tombstone. Never compaction, so handles never relocate.
- **geometry change** — in place, identity preserved. Invalidates the
  cached input image and forces a re-init (I, then full T), because the
  NI/NO announced in the I body has changed.
- **enable / disable** — unchanged from v1.1.

Address is identity, so changing a node's address is delete + add,
never an in-place mutation. A handle that silently became a different
logical device is worse than one that dangles.

Handle lifetime: `host.node(addr)` is the canonical access path and is
cheap enough to call at point of use. A handle is valid until that node
is deleted. Caching across a mutation is not a supported pattern;
`address()` is the self-check for code that does it anyway.

Mutating the node of the outstanding exchange is legal. A send in
flight cannot be aborted — bytes are on the wire and TXEN is asserted,
so truncating would violate D13's transmit-drain doctrine. The exchange
is **orphaned** instead: the frame completes, any reply is discarded,
and nothing is attributed to any node. Without an explicit orphan mark,
a late reply is matched against whatever now occupies the slot.

Slot reuse resets all three substrates (D15). Freshness in particular
must be cleared, or a newly added node reports data it never sent.

Failure surface: every mutator returns its own status immediately.
There is no sticky, chain-poisoning status and no chaining idiom. That
was a hardcoded-test-rig ergonomic; the verb-based C&C regime
invalidated its premise, and in practice it let one rejected add
silently disable every later add.

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
Allocation discipline (moved here from D5 in v1.2, being an
implementation-profile concern rather than a lifecycle one): the
embedded profile allocates only during setup and never frees. `begin()`
may allocate; nothing after it does. This applies to transports as
well. A desktop Host build may satisfy the same *functional* contract —
bounded capacity (D8) and a well-defined failure surface (D5) — with
whatever storage strategy it prefers.
Observability is listener registration (JMRI pattern): metrics,
monitor, and trace hooks are optional listeners the linker drops when
unused. No feature `#ifdef`s inside the library. Platform guards on
platform-specific ports (e.g. `#if defined(ARDUINO_ARCH_ESP32)` on
`Esp32UartCMRISerialPort`) are not feature toggles — they are the only
mechanism the Arduino build model offers for a port that calls into a
core-specific driver, and a non-matching build sees an empty file
(the shipped guard is `#if defined(ARDUINO) && defined(ARDUINO_ARCH_ESP32)`).

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
UA verification, buffer sizing (frame decode at the 256-byte protocol
ceiling; per-node reply images default to JMRI's 118-data-byte
ceiling so accepted geometries never silently fail under the dominant
fielded Host, knob-raisable to 128 — the fielded maximum — or 256 for
conformance builds; 2x TX staging), and
recovery rules implement `docs/cmrinet-interop-profile-and-errata.md`
Part 2. Where the profile and the spec text disagree, the profile
wins, and the erratum (Part 1) records why.

### D11. The MQTT carrier transport is the third-party seam proof
A `MqttCMRITransport` implemented by an independent party, against
the transport contract alone, validates the packet seam. Acceptance:
the desktop loopback rig (CMRIHost to CMRINode) runs over it with
zero engine changes and all mock-transport suites still passing.
Dispatch only after the codec, engine, and mock transport exist.
Speed is explicitly not a goal. It is a proof, not a product — see
D12 for the product-shaped MQTT story.

### D12. The semantic gateway is the product's first application
An ESP32 gateway device runs `CMRIHost` at the RS-485 segment
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

### D13. Inter-byte abort doctrine
The receive inter-byte abort limit (interop 2.2.6) has two distinct
roles, kept apart:
- **Shipped/deployment default: tolerant.** `SerialCMRITransport`
  ships `kShippedInterByteTimeoutMs` (order 100 ms). The reference
  Host lineage transmitted with interpreter-scale gaps (interop 2.2.6),
  and fielded Nodes pace with dH/dL (erratum E4), so a strict shipped
  default would fail conforming history. The 250 ms reply gate (D2,
  D9) is the truncation backstop; the abort is a framing defense, not
  the truncation guard.
- **Rate-derived value: a conformance instrument, never a default.**
  `rateDerivedInterByteTimeoutMs()` returns three character times
  (interop 2.2.6). A tracer or conformance scenario opts in by passing
  it to `setInterByteTimeoutMs()`. It is never a compile-time or
  shipped default.

Concrete rule: the abort limit must exceed the worst node's configured
dH/dL per-character delay plus margin (E4) — otherwise a legally-paced
classic node trips the Host's own abort. That couples the abort to
per-node config in principle; the natural home for a per-node abort
override is `CMRIHost::RemoteNodePolicy`, which already exists today
with a `replyTimeoutMs` field. No per-node abort-timeout field exists
yet, and none is needed today: the transport decodes a frame before its
UA is known, so the abort is transport-wide, and the tolerant shipped
default (100 ms) exceeds the fielded dH/dL ceiling by construction —
AVR Nodes cap the honored delay near 16.4 ms (E4), so 100 ms covers
any legally-paced node a Host has configured under the JMRI-tuned
defaults (D9) with roughly 6x margin. Add a per-node field only when a
consumer whose nodes exceed that ceiling appears.

Companion: the `transmitDrained()` seam contract (CMRISerialPort.h)
and the TXEN two-gate drain detector (SerialCMRITransport) are the
transmit-side doctrine this receive-side doctrine pairs with. The
conjunction (estimate AND port drain) is kept even for hardware-truth
ports: the estimate never outlives a real drain, so it costs nothing.

### D14. Declared geometry is a claim; observed geometry is evidence
There are three sources of truth about a Node's geometry, and under the
current spec only one direction flows:
- **Declared** — `RemoteNodeConfig.inputBytes/outputBytes`.
- **Announced** — the NI/NO fields of the I body (interop E3).
- **Physical** — the Node's actual card complement.

Declared flows to announced; nothing returns. That yields a
truth-acquisition ladder:
- **L0** declared only — config is unfalsifiable.
- **L1** inferred — reply length reveals actual NI. Available today.
- **L2** negotiated — an I-ack carrying NDP/NI/NO (issue #34).
- **L3** discovered — a self-identify MT (issue #35).

**Input geometry is observable; output geometry is not.** A Host can
catch an over-declared `inputBytes` from an R length, but an
over-declared `outputBytes` is undetectable until L2. Design for the
asymmetry rather than around it. L1 populates the same fields L2 and L3
later populate more precisely, so building the slot now is
forward-compatible, not a stopgap. Note also that a fielded Node which
ignores I never sends an I-ack, so L2 cannot cover the installed base:
L1 remains the only universal mechanism.

Faults are classified on two axes, not one flag:
- **Layer**, indexed by the units ladder: image (geometry, NI/NO),
  packet (UA, MT, malformed body), framing/carrier (DLE, truncation,
  inter-byte abort). The image rung is strategy-invariant; the bottom
  rung is carrier-specific and is replaced, not merely renamed, by a
  message carrier (D11).
- **Attribution**: a **defect** is the counterparty violating the spec;
  a **disagreement** is two correctly-behaving endpoints configured
  inconsistently. Geometry mismatch is a *disagreement* — the Node is
  doing exactly what it was built to do, and the remedy is a config
  fix, not a firmware fix.

One flat `ConformanceFault` enum names specific faults; free
`layerOf()` / `attributionOf()` classifiers derive the axes, so invalid
combinations cannot be constructed and call sites do not chain field
tests. The vocabulary is role-neutral and carries no poll terms, so
both roles share it: the polled engine's `ReplyRejectReason` maps into
it rather than being promoted to it.

**Detection and attribution are different layers** (v1.3). Layer says
where a fault was *observed*; attribution is a *verdict* about what it
means. For most faults the two collapse, because the fault's name
already encodes the verdict. Where they do not collapse, that is a
property of the fault, not a gap in the taxonomy.

Attribution is derived, not asserted. A fault is conceptually a tuple
of (Host assumption, Node assumption, observation):
- assumptions differ → **disagreement**
- assumptions agree and the observation contradicts them → **defect**

The tuple produces the attributions above rather than stipulating them.
Geometry mismatch *means* the assumptions differed. An unexpected
message type *means* they agreed and the content violated them. An
unexpected address means the Node believes it holds a different address
— and the residual ambiguity (another Node answering out of turn) is
not ambiguity about the fault but about *whose* assumption set is being
compared, which the Host cannot always know.

Admission test for a new fault: it may be classified statically if and
only if its name already encodes the assumption comparison. "Geometry
mismatch" and "unexpected type" do. "Truncated" does not.

**A framing fact is not yet a conformance fault.** A malformed frame is
a faithful observation — the frame really was malformed — but it carries
no causation, and at the framing rung a Node that stopped mid-frame is
indistinguishable from a frame the wire corrupted. Attribution needs
history, so it belongs one rung up: framing answers yes/no, analysis
calls the shot.

Determinism is what makes that analysis sound. Fielded implementations
are deterministic products of a repeatable code path, not arbitrary
packet sources, so:
- malformed on every occurrence of a stimulus → a deterministic code
  path → **defect**;
- malformed only on particular content (bodies containing 2/3/16, the
  DLE-escape trap) → a content-dependent code path → **defect**;
- the Host's abort limit below the Node's legal dH/dL pacing (D13) →
  the assumptions differ → **disagreement**, and a configuration check
  rather than a statistical one;
- malformed sporadically with no stimulus correlation → cannot be a
  deterministic code path, therefore not a firmware defect.

That last case is **not an unattributable conformance fault**. It is a
link-integrity finding — cabling, termination, grounding — which has a
real owner and an existing home in `LinkStatistics` and
`CMRIFrameDecoder::Statistics`. Conformance attribution therefore stays
two-valued: there is deliberately no "environment" or "indeterminate"
attribution, because either would import another domain's verdict into
this one and read as "not my problem".

Consequence worth stating, because it keeps the classifier honest: when
analysis promotes a framing fact into a *named* conformance fault, it
has already excluded the disagreement and link-integrity readings, so
the remaining verdict is a defect by construction. Named framing faults
are therefore statically classifiable, and the admission test above is
satisfied at the moment the name is created. The framing rung stays
empty until an analysis layer exists to populate it.

### D15. Three state substrates, and an invariant
Per-node state divides into three kinds, and conflating them is what
makes protocol drivers fragile:
1. **Control** — protocol state-machine guards: init/re-init flags,
   deadlines, backoff, dirty-output, miss counts. Mutable, resettable,
   drives behavior. Largely strategy-specific.
2. **Belief** — the product surface: input image, output image,
   freshness.
3. **Observation** — monotonic counters and last-fault detail.
   Reporting only.

**Invariant: control state is never read from observation, and
observation never gates behavior.**

v1.1 violated this: `consecutiveMisses` was documented as a never-reset
statistic while resetting on accept and gating both OFFLINE and the
re-init ladder. `RemoteNodeConfig.enabled` is the same category error
in the other direction — runtime control state living in a config type
— and is retained deliberately, recorded here so the exception stays
visible.

The split also gives slot reuse (D5) a checklist rather than a hunt,
and keeps the health axes of D16 computable from independent inputs.

Clarification, because slot reuse reads like a violation and is not:
D5 resets all three substrates on reuse, observation included, and
observation is supposed to be monotonic and never reset. Monotonicity
is a property of a counter **for a given subject**. `delete` ends that
subject; the next occupant of the slot is a different logical device,
so it gets a new counter starting at zero. Nothing was reset mid-life.
This is the invariant applied correctly, not an exception to it — the
only recorded exception remains `RemoteNodeConfig.enabled` above.

### D16. Node health is three axes plus a projection
Supersedes D2's claim that `RemoteNodeState` is a stored four-value
enum. It was already an undocumented projection: UNINITIALIZED and
STALE are statements about data validity, OFFLINE is a statement about
liveness, and because liveness came first in the ladder it masked the
validity reading entirely.

Stored axes, one per substrate (D15):
- `RemoteNodeLiveness` — responsive / missing / silent (control).
- `RemoteNodeImageState` — none / fresh / stale (belief).
- `RemoteNodeConformance` — unknown / conforming / nonconforming
  (content evaluation).

`RemoteNodeState` is retained as the *derived* single-scalar projection
for displays and simple consumers, extended with MISCONFIGURED
(nonconforming with no valid image) and DEGRADED (committing data while
faulting).

Conformance is **current, not latched**: it may only be asserted from
current evidence, so loss of contact degrades it to unknown. A node
cannot therefore be simultaneously OFFLINE and nonconforming, which
makes those two projection values mutually exclusive and removes any
ordering question between them. History is not lost — it lives in the
observation substrate as a fault count and last-fault detail.

The lifecycle is chronological rather than a free cross-product:
OFFLINE → UNINITIALIZED → ONLINE → DEGRADED/MISCONFIGURED → OFFLINE.
A node that goes nonconforming reaches STALE first, because rejected
replies stop refreshing freshness; it reaches MISCONFIGURED only when
re-init invalidation clears freshness (interop 2.3.10). MISCONFIGURED
is therefore reachable *only because* D17's breaker attempts bounded
re-init before tripping.

Two predicates answer the two questions consumers actually ask, so no
call site re-derives them: `isHealthy()` (operator: live, fresh,
conforming, breaker closed) and `inputsUsable()` (application: may I
act on this image now). They diverge in both directions.

### D17. Degraded nodes are a service class with a bounded budget
A Host serves a layout that evolves organically: nodes are added
mid-construction, unplugged, rewired, and misconfigured, while the rest
of the layout must keep working. Degraded nodes must therefore have
bounded, shared, predictable impact on healthy ones.

This introduces the first priority ranking into a deliberately flat
round-robin: two service classes, healthy and degraded, with different
guardrails. The scheduler admits degraded work through a rate-limited
lane governed by **two gates**, both of which must pass:
- **Rotation slots** — bounds participation share and trace noise.
  Nonconforming-but-answering nodes bind here: they cost a full turn.
- **Wall-clock bandwidth** — bounds cycle latency for healthy nodes.
  Silent nodes bind here, costing a full reply gate per probe.

The asymmetry is measured, not assumed: a silent probe costs the reply
timeout (order 250 ms) while an answering probe costs turnaround (order
15-20 ms). A single time budget misses the first failure mode; a single
slot budget misses the second.

Liveness backoff and the conformance breaker are distinct mechanisms
that share this allocator rather than holding independent timers. Where
both apply, the gates AND. `maxPollBackoffMs` is demoted from primary
knob to ceiling clamp; the operator-meaningful knob is the budget.

The **conformance breaker** trips after a bounded number of corrective
re-init attempts. It is never a hard stop: a node reflashed with
correct firmware must recover **without restarting the Host**, and zero
traffic means no evidence of recovery can arrive. It therefore probes
at a low rate with a bare P — never a re-init sequence, whose post-I
settle would stall the round-robin — and re-closes on either a
conforming reply or a runtime change to the declared geometry, arming
the re-init ladder on close because a reflashed node has lost its
session. If a breakered node falls silent, that is the liveness path's
job, not the breaker's.

Invariant: **the degraded class is never starved to zero**, or recovery
is never observed. The ceiling clamp guarantees this and may
deliberately exceed budget — slightly over budget beats never noticed.

Rationale for failing fast rather than polling on: in a static-firmware
world where Host and Node share an externally supplied configuration
agreement, there is effectively no such thing as a transient geometry
mismatch. The population is dominated by permanent errors from a
miscompiled sketch or a mis-set Host, so the default path must be tuned
for the field case, not the test case. This is spec-legal: interop
2.3.10's "poll the silent Node forever" is a liveness obligation and
says nothing about a Node whose replies are structurally unusable.

## Transport contract (packet seam)

Scope: transports for the CMRInet polled strategy. Not a product-wide
layer (see D4, one-product model).

```cpp
namespace CMRInet {

class CMRITransport {
 public:
  virtual void begin() = 0;                        // may allocate (D7)
  virtual void tick(uint32_t nowMs) = 0;           // sole CPU entry; never blocks
  virtual bool sendPacket(const CMRIPacket&) = 0;  // accepted != on the wire
  virtual bool sendComplete() const = 0;           // true once fully delivered
  virtual bool receivePacket(CMRIPacket&) = 0;     // whole validated packets only
  virtual const LinkStatistics& stats() const = 0; // link errors and liveness
};

}  // namespace CMRInet
```

Clause semantics (normative for implementers):
- `sendPacket()` must not block. It returns false only for
  backpressure or link-down. The caller retries on a later tick.
- `sendComplete()` gates the strategy's reply timer. Serial: the last
  byte left the shift register and TXEN dropped — detected
  non-blocking as the wire-time estimate for the accepted bytes having
  elapsed AND the port reporting its transmit path empty (interop
  2.3.14), never a blocking flush (D6). A hardware-truth port tightens
  this; a buffer-only port is covered by the estimate. Message
  transports: the client accepted the message for delivery.
- `receivePacket()` returns packets in arrival order, at most one per
  call, and only packets that passed the transport's integrity checks
  (framing, escaping). Address filtering is NOT the transport's job.
- All client work (UART pump, MQTT keepalive, reconnect) happens
  inside `tick()`, non-blocking.
- Link liveness and error counters surface through `stats()` in
  transport-neutral terms, so the strategy folds them into node
  health without knowing the medium.
- Allocation only in `begin()` (D7).
- Lifecycle and ownership: the sketch constructs and configures the
  transport; the engine calls `transport.begin()` exactly once, from
  its own `begin()`. Sketches do not call the transport's `begin()`.
  `begin()` establishes a clean initial state (buffers, stats, and —
  in mocks — replay scripts). Test consequence, learned in #6: script
  mock replies after `CMRIHost::begin()`, never before.

Known implementer decisions for a message-carrier transport (MQTT):
1. Topic scheme: per-UA topics are legal and preferred. The engine
   never assumes a shared medium.
2. Payload: naked `{UA, MT, body}` versus fully framed bytes. Framed
   payloads make a serial bridge a dumb byte pump. Choose and justify.
3. QoS/retain mapping: QoS 0 mirrors the wire. A retained output
   image gives late joiners the last T. Document the choice.

## Handle contract (strawman, pre-implementation)

```cpp
// ---- HOST sketch (bench, gateway) ----
CMRInet::CMRIHost me(transport);                // config phase
CMRInet::RemoteNodeHandle& node5 =
    me.addRemoteNode(ua,
        CMRInet::RemoteNodeConfig{...},         // neutral: staleness, enabled
        CMRInet::CMRIHost::RemoteNodePolicy{...}); // polled knobs (optional)
me.onEvent(fn);                                 // timeouts, re-inits, errors
me.onTrace(fn);                                 // TX/RX packets (monitor)
me.begin();                                     // config -> running (D5)

me.tick(nowMs);                                 // runtime, non-blocking

node5.setOutputBit(bit, v);                     // marks dirty -> full-image T
node5.setOutputs(image, len);
bool b       = node5.inputBit(bit);             // last good IB
uint32_t age = node5.inputAgeMs(nowMs);         // staleness (D2)
CMRInet::RemoteNodeState s = node5.state();     // projection of 3 axes (D16)
const CMRInet::RemoteNodeStatistics& st = node5.stats();
node5.setEnabled(false);                        // delete is also legal (D5)
```

```cpp
// ---- NODE (device) sketch ----
CMRInet::CMRINode me(transport,
    CMRInet::CMRINodeConfig{ .ua = 5, ... });   // engine's OWN types
me.onOutputs(applyOB);                          // OB arrived -> drive pins
me.begin();
// loop(): me.setInputBit(bit, v); me.tick();
// no RemoteNodeHandle here — a device holds no views of other nodes
```

The families cannot blur: a Host sketch manages *other* nodes through
the `RemoteNode*` product family; a device sketch IS the machinery
and touches only `CMRINode` and its own `CMRINode*` types.

Note the strawman above predates the implementation: `addRemoteNode()`
returns the host for chaining and never hands back a handle. Under D5
that chaining idiom is retired in favor of per-call status, and handles
come from `node(addr)` at point of use.

Open items to settle during tracer-bullet implementation:
1. Default output semantics: T-on-change (JMRI) per D9. Confirm on
   the bench. `forceTransmit()` exists either way.
2. Per-node input-change callback, or polled-only handle. Start
   polled-only. Add the callback only if diff-scanning hurts.
3. Counter granularity for conformance faults (D14). Deferred
   deliberately: events carry layer, attribution, and expected/actual,
   so the bench analyzer aggregates externally and real failure
   distributions decide which cuts earn a durable counter. Until then
   the residue is one total plus last-fault detail.
4. Warty-Node trait vocabulary (D3 fidelity 3). Traits are
   individually toggleable rather than a mode enum, because isolating
   one defect at a time is the point, and because the verb-based C&C
   regime drives them — so trait identifiers are part of the C&C
   vocabulary, mapped in one place rather than scattered comparisons.
   `ignore-init` and `tolerant-geometry` join the traits drawn from the
   research reviews. The strict Node (fidelity 2) is the default: a
   forgiving counterparty absorbs Host bugs and defeats the test rig.

## Test strategy

- Unit: codec against byte vectors, including every pathological case
  in `comparison.md` §3 (the anti-checklist is the test plan). Engine
  against mock clock plus scripted-replay transport (fidelity 1).
- Integration: desktop loopback, CMRIHost to CMRINode over paired
  mock transports (fidelity 2), later over `MqttCMRITransport` (D11).
- Conformance/bench: warty CMRINode (fidelity 3), fault injection,
  slow byte-spaced TX (classic Hosts sent gapped bytes; see profile
  2.2.6), oversized frames, truncations.
- Hardware: the PLAN.md two-board bench (Xiao Host plus Xiao_I2C
  node) validates TXEN timing and real-wire behavior mocks cannot.

## Scope for the tracer bullet (Phase 1, revised)

In: codec, `SerialCMRITransport`, `MockCMRITransport`, CMRIHost
(I/T/P), `RemoteNodeHandle` (inputs, outputs, freshness, state, re-init
ladder), scripted-replay tests, OLED hit/miss display per PLAN.md.
Out (sequenced, not abandoned): CMRINode emulator (Phase 2, as the
loopback counterparty), MQTT carrier proof (after Phase 2), semantic gateway
app (after the emulator), push strategy (no consumer), SUSIC/SMINI node
types (bench roadmap).
