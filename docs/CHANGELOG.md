# Changelog

High-level changes, newest first.

## Unreleased

### Added
- Node example sketches (issue #9, M5): three examples on the new
  transport include shape.
  - `examples/SimpleNode/SimpleNode.ino` — the front-door Node
    tutorial: minimal GPIO via direct accessors in `loop()`, no
    pack/unpack callbacks needed for simple cases.
  - `examples/TracerNode/TracerNode.ino` — the bench C&C Node test
    mule: JSON-lines packet trace, ring-buffer capture (run/dump/reset),
    status verb, OLED identity.
  - `examples/XiaoNode/XiaoNode.ino` — the full-featured Node:
    OLED status panel with OTA progress/success/error screens, WiFi
    OTA via the donor `ota.h/.cpp` module (unchanged), MCP23017 I2C
    expander I/O through the pack/unpack callback seam. No cpNode
    library dependency — MCP23017 access is inline via Wire.h.
  - `sketch_lint.py` covers all three new sketches (8 total).
- CMRINode: pack/unpack callbacks no longer called with len==0. The
  engine guards `handlePoll_` (skips pack when `inputBytes == 0`) and
  `handleTransmit_` (skips unpack when the T body is empty). Callbacks
  can assume len > 0.
- ADR-0004: library boundary and transport header packaging. This
  library is the CMRI strategy: packets, byte images, and the two CMRI
  engines, over pluggable packet carriers. The semantic/point layer
  belongs to a sibling library. MQTT-as-carrier is dropped (no
  consumer; the seam is proven by mock + serial + planned TCP). TCP is
  the alternate carrier (JMRI `networkdriver` interop). Transport
  headers move to `src/transport/` with prefix-dropped filenames; the
  umbrella stops including implementations. Packaging rule: directory
  = seam, filename = discriminator, generic names in subdirectories
  (Arduino PR #1853 collision-resolution rationale). DESIGN bumped to
  v1.7 (D11 obsolete, D12 rescoped, D1/D3 strengthened, layer model
  rescoped, Node strawman replaced, packaging rule added). HANDOFF.md
  deleted (rottod, forbidden vocabulary). library.properties fixed
  (retired PolledHost/PolledNode names). sketch_lint.py expanded to
  cover XiaoBenchCal and XiaoBenchEchoCancel.
- Baseline CMRINode engine (issue #9, M1-M4, Design D3): the
  polled-strategy Node role. Receives addressed I, T, and P; emits R in
  reply to P. I and T expect no reply (interop E8). The Node serves one
  UA, so there is no schedule or round-robin.
  - New `CMRInet::CMRINode` class and `CMRINodeConfig` struct
    (`src/CMRINode.{h,cpp}`). Config carries `ua`, `nodeType` (the NDP,
    default `'C'`), `inputBytes`/`outputBytes` (NI/NO), and
    `transmissionDelayDh`/`Dl`.
  - The pack/unpack seam: function-pointer + context handlers called at
    P time (fill the input image) and T time (drive pins from the output
    image). Complementary direct accessors (`setInputBit`/`setInputByte`/
    `inputBit`/`inputByte`/`outputBit`/`outputByte`) touch the same image
    buffers anytime. Both patterns work alone or together (M3).
  - `onTrace` optional packet trace listener (D7, same as Host's).
  - `begin()` idempotent; `tick(nowMs)` non-blocking (D6). Blocking is
    bounded, not prohibited: the Node's only obligation is to produce R
    before the Host's reply gate (~250 ms); a 2 ms I2C read inside
    `pack` is fine.
  - No packet visibility, no card-type awareness, no opts handling (D4).
    #34/#35 (I-ack, bus discovery) are future follow-ons; the config is
    shaped to support them but the paths are not built.
  - 7 desktop loopback integration tests (`tests/test_node.cpp`): I
    reaches Node, T reaches Node, P→R round trip, full Host↔Node
    loopback, UA mismatch discarded, NDP mismatch discarded, set input
    bit. 290 total tests passing.
  - `src/CMRInet.h` includes `CMRINode.h`; `tests/Makefile` builds
    `test_node`.
- Packet-layer illegal wire-UA detection (issue #96, Design v1.6 D14):
  illegal wire-UA bytes (outside [65, 192]) are now detected at the
  Host's packet-verify boundary, before the solicited/unsolicited split,
  and classified as the first absolute packet-rung fault.
  - New `ConformanceFault::kPacketIllegalWireUA` (layer=packet,
    attribution=defect). Distinct from `kPacketUnexpectedUA` (a
    legal-but-wrong UA, a disagreement): illegal is not "wrong," it is
    "not a UA."
  - New `isLegalWireUA(uint8_t)` predicate in `CMRIPacket.h`: pure,
    stateless, shared by the Host gate, the tracer, and the tests.
  - New `CMRIHostStatistics::illegalWireUAFaults`: host-scope,
    monotonic. An illegal UA names no node to charge, so this counter is
    the only surface on which a chronic illegal-UA emitter becomes
    visible.
  - New `CMRIHostEventType::kIllegalWireUA`: fires with `node = null`
    and the raw wire byte, the same null-node pattern `kNodeDeleted` uses.
  - The 4b miss behavior: an illegal UA during an outstanding POLL's
    reply gate does not satisfy the poll. The gate stays armed, times
    out, and the polled node takes the miss. `illegalWireUAFaults` and
    the miss ladder climb together at the poll rate, which localizes a
    chronic offset-omission emitter no per-node attribution can name.
  - Tracer trace lines now carry a uniform field set:
    `wireUA`, `legal`, `UA` (decoded ordinal or null). No variable
    stream that swaps between telemetry and raw.
  - Generator parser keys are now case-insensitive (folded from #102).
  - 10 new tests across `test_conformance_fault`, `test_host`, and
    `test_tracer`.

### Changed
- Transport headers moved into `src/transport/` and the umbrella
  `CMRInet.h` stopped including implementations (ADR-0004, breaking).
  `MockCMRITransport.{h,cpp}` → `transport/mock.{h,cpp}`,
  `SerialCMRITransport.{h,cpp}` → `transport/serial.{h,cpp}`,
  `CMRISerialPort.h` → `transport/serialPort.h`,
  `StreamCMRISerialPort.h` → `transport/serialStream.h`,
  `Esp32UartCMRISerialPort.h` → `transport/serialESP32.h`.
  `CMRITransport.h` (the seam) stays at top level; type names are
  unchanged. A sketch now includes the transport it chooses:
  `#include "transport/serialESP32.h"` (which pulls `serial.h`). The
  umbrella carries the seam, packet, codec, time, engines, and handles
  — but no transport implementations. All internal headers, tests,
  desktop tracer, and sketches updated.
- Bit accessors changed from flat bit index to `(byte, bit)` signature
  (issue #9, breaking). `RemoteNodeHandle::setOutputBit(bit, v)` →
  `setOutputBit(byte, bit, v)`, `inputBit(bit)` → `inputBit(byte, bit)`,
  and the same for `outputBit`. `CMRINode` follows the same shape for
  `setInputBit`/`inputBit`/`outputBit`. The flat index required the
  caller to do `byte * 8 + bit` math; the `(byte, bit)` form matches how
  the images are actually addressed and is used internally across the
  repo. All call sites updated: `RemoteNodeHandle`, `CMRINode`,
  `TracerShell`, `test_host`, `test_tracer`, `test_node`, `SimpleHost`,
  `XiaoHostTracer`.
- CLI `--address` retired end-to-end (follow-up to the #102 rename).
  The #102 rename deliberately kept `--address` for backward compat,
  but the rest of the bench tooling had already migrated to `--ua`,
  so the lone `--address` was now inconsistent with its own family.
  - `cmri_tracer --address N` is now `cmri_tracer --ua N`.
  - `bench resolve --id|--address` drops the `--address` alias; the
    canonical flag is `--id`. The alias was `--id`, not UA-specific:
    the role ID is "RX"/"TX" for sniffers and a UA only for Node
    roles, so `--ua` would have misdescribed it. `bench resolve
    --role Node --id 30` is unchanged in behavior.
  - Callers updated; no fielded external consumers.
- UA terminology rename (issue #102, breaking): the library's
  `ua`/`address` vocabulary was inverted relative to the field norm.
  Bruce Chubb's QBASIC reference code and NMRA LCS-9.10.1 both name the
  ordinal (0..127) `UA`; the on-the-wire byte is `UA + 65` and unnamed.
  The library labelled the wire byte `ua` and the ordinal `address` —
  swapped relative to the field, while the arithmetic was correct. The
  rename makes the names honest, and capitalizes `UA` in all
  identifiers to match the field convention:
  - `ua`/`ua_`/`ua()` → `wireUA`/`wireUA_`/`wireUA()` (the wire byte,
    `UA + 65`)
  - `address`/`address_`/`address()` → `UA`/`ua_`/`ua()` (the ordinal,
    0..127)
  - `kUaOffset` → `kWireUAOffset`, `kAddressOutOfRange` →
    `kUAOutOfRange`, `kAddressInUse` → `kUAInUse`, `departedAddress` →
    `departedUA`, `kPacketUnexpectedAddress` → `kPacketUnexpectedUA`,
    `replyUa` → `replyWireUA`, `TRACER_ADDRESS` → `TRACER_UA`.
  - JSON telemetry field names (`"ua"`) are unchanged — they already
    spoke the semantic UA. The CLI option `--address` is unchanged for
    backward compat.
  - ADR-0001 stays Deferred; this advances the naming half of its
    boundary. The deeper question — should `RemoteNodeHandle::wireUA()`
    exist on a strategy-neutral product surface at all — still waits on
    #9.

### Added
- Testable CDC line writer (issue #99): the chunked `writeCdcLine`
  logic is pulled out of `examples/XiaoHostTracer/XiaoHostTracer.ino`
  into `src/testbed/CdcLineWriter.h` behind a `CdcConsole` seam (open,
  room, write, clock, yield), so the chunking, the per-call room check,
  and the reserved-terminator-budget split run under the desktop
  `-Werror` gate and a fake port. The logic is identical to the
  sketch-local copy; only the location changes. A `LineWriter`
  trampoline (`writeCdcLineCb`) lets the shared engine bind the shared
  logic directly. `tests/test_cdc_line.cpp` covers the happy path, a
  chunked body, the #86 full-buffer case (the terminator lands when the
  buffer was full through the body budget), a closed stream, and
  body-budget exhaustion with the reserved terminator slice. The sketch
  keeps only a `XiaoCdcConsole` adapter bound to `Serial`, `millis`,
  and `delay`. The desktop tracer is unchanged: its `puts` and `fflush`
  writer has no bounded buffer and no full-buffer problem. On-bench
  validation: `extras/bench/validation/cdc_line/gather_cdc_backpressure.py`
  slow-reads the live CDC trace stream to fill the USB ring, then checks
  no records merged (the #86 signature) and that `final` lands; under a
  227x backlog it recorded 1810 valid records, 0 merges, `final` landed.
  See [docs/cdc-line-writer-bench-findings.md](docs/cdc-line-writer-bench-findings.md).
- Sketch warning gate for the examples (issue #93): `make sketch-lint`
  compiles every example sketch under `-Wall -Wextra -Werror -Wswitch`,
  compile-only, with warnings bound to our code only. The ESP32 core
  suppresses warnings for the sketch translation unit (it appends `-w`
  *after* our flags, and last wins), and no `arduino-cli` invocation
  switches them back on, so an unhandled enumerator — or any ordinary
  warning — in a sketch shipped silently. `RemoteNodeState` grew
  `kMisconfigured`/`kDegraded` under #84 and two sketch copies of the
  state switch rotted to `"??"` on the same commit, unnoticed, for
  exactly this reason. The gate harvests each sketch's real
  cross-compiler command via `arduino-cli compile
  --only-compilation-database`, then recompiles the sketch translation
  unit with `-w` dropped, `-Wall -Wextra -Werror -Wswitch` added, and
  every third-party include dir (esp32 core, tools, Wire/SPI, Adafruit
  GFX/BusIO/SSD1306) demoted to `-isystem` so their header noise is
  suppressed while `src` and `examples/<sketch>` stay on `-I`. Board
  `-D` defines are kept verbatim (notably `-DARDUINO_USB_CDC_ON_BOOT=1`,
  which overriding `build.extra_flags` would clobber). No hardware
  needed — compile-only, `-o /dev/null`, no flash. New top-level
  `Makefile` adds `sketch-lint`, `test`, `desktop`, and `check` (all
  three gates) targets; `extras/bench/flash_and_probe.sh` runs the gate
  as a pre-flight before flashing. See
  [docs/sketch-warning-gate.md](docs/sketch-warning-gate.md). Whether
  the gate also runs in CI is deferred to map #72; it leaves a clean
  seam (one script, non-zero exit) for that. The shared-helper half of
  #93 (both sketches call `remoteNodeStateTag()`, `TracerShell`
  delegates) landed under #85; this ticket lands the gate that makes a
  revert of that delegation fail a documented check.
- Host events for runtime node-table mutation (issue #91, Design v1.2 D5):
  `addRemoteNode`, `deleteRemoteNode`, and `setRemoteNodeGeometry` now fire
  `CMRIHostEvent`s, so a listener sees the table change whether the mutation
  came from a C&C verb or a direct API call. A mid-run topology change was
  previously invisible in the event stream — only the status roster hinted
  at it — so an operator reading a capture could not tell "the node was
  deleted" from "the node went silent," which are different faults with
  different remedies. This matters most for #88, whose captures are the
  artifact under analysis.
  - Three new `CMRIHostEventType` values: `kNodeAdded`, `kNodeDeleted`,
    `kGeometryChanged`. `kNodeDeleted` carries the departing identity by
    value (`departedAddress`) because the slot is cleaned before the event
    fires and `node` is null by design; `kGeometryChanged` carries both the
    previous and the new NI/NO (`previousInputBytes` / `previousOutputBytes`),
    so a reader can tell what changed without dereferencing the handle.
  - **Mutation rendering is unified through the engine.** `TracerShell`'s D5
    verb handlers become thin wrappers (parse, call the mutator, emit only
    the error line on rejection); on success the engine event renders via
    `onHostEvent_` through the same `emitNodeLine_` / `emitHostLine_` the
    old verb acks used. The event payload is a superset of the old ack, so
    the line shape is unchanged for add/delete and gains `previousIn` /
    `previousOut` for geometry. One source of truth, one render path.
  - **No event fires on a rejected mutation.** Each mutator returns its
    `ConfigStatus` before reaching the listener dispatch, so a failed call
    produces only the shell's error line, not a spurious event. Validation
    lives in the mutator; the shell's redundant `ua > 127` pre-check on the
    mutation verbs is removed, and an out-of-range UA now surfaces as
    `addFailed` / `deleteFailed` / `geometryFailed` with the precise reason.
  - **Mutator timestamps are last-tick, guarded.** Mutators run outside
    `tick()` and have no clock of their own, so mutation events stamp
    `nowMs` from a `lastTickMs_` captured at the top of `tick()`. A mutation
    before the first tick honestly stamps 0 ("no clock yet") rather than a
    fabricated value; pre-`begin()` adds are a legitimate boot-time path and
    are not refused.
  - 5 new `test_host` tests (delete names the departing node, runtime add
    fires `kNodeAdded`, geometry carries old + new NI/NO, rejected mutations
    fire nothing, pre-tick mutation stamps zero) and 2 new `test_tracer` tests
    (geometry line carries `previousIn` / `previousOut`; a direct-API delete
    renders the `node_delete` host-scoped line through the bound shell).
- Degraded service classes and the conformance breaker (issue #87, Design
  v1.5 D16/D17). **This closes the #80 bug.** A node declared `NI=4`
  answering with 3 bytes took 1316 polls to a healthy node's 765 — 63% of
  all poll slots, serviced 1.7x more often than the node doing real work,
  while committing nothing. #85 gave that node its right name
  (`MISCONFIGURED`); this gives it a bounded cost.
  - **Two gates, because the cost asymmetry is measured.** A silent probe
    burns a full reply timeout (~250 ms) and one slot; an answering
    nonconforming probe burns turnaround (~15-20 ms) and one slot. One
    60 s capture holds both on one bus at 7 polls against 1206 — a 172x
    gap. Gate A bounds rotation slots, Gate B bounds wall-clock
    bandwidth, and both must pass. Either alone misses one failure mode.
  - **The gates engage only when a healthy node is contending.** Their
    justification is protecting healthy nodes, so where there are none
    they protect nothing and would only delay recovery — a lone silent
    node would jump from the 250 ms backoff ladder straight to the
    ceiling clamp on its first miss.
  - **`maxPollBackoffMs` is demoted from primary knob to ceiling clamp**,
    and is now what guarantees the degraded class is never starved to
    zero. It may deliberately exceed budget: a class served at zero rate
    can never demonstrate its own recovery.
  - **The conformance breaker gets a writer.** A run of nonconforming
    replies arms bounded corrective re-inits, then trips. A tripped
    breaker probes with a bare `P`, never a re-init — the post-`I` settle
    would stall the round-robin — and re-closes on a conforming reply or
    a runtime geometry change. `conformanceBreakerOpen_` had existed
    unwritten since #85; `isHealthy()` and `inputsUsable()` now diverge
    with every axis set, as D16 predicted.
  - **The corrective re-init is load-bearing for correctness, not
    courtesy.** D16 v1.4 said MISCONFIGURED does not depend on the
    breaker. That is right for two of the three paths and wrong for the
    third: a node that conformed and then went nonconforming *while still
    answering* never accumulates a miss, so interop 2.3.10's
    silence-armed ladder can never fire and nothing else clears
    freshness. It parks at STALE with a growing age — "your data is old"
    standing in for "your geometry is wrong", the same concealment #80
    was filed about. That is the organic-rot case (cards rearranged,
    sketch recompiled, node alive on the bus), so it is the likeliest
    field path, not an edge case. D16 is narrowed to v1.5 rather than
    reverted, and the engine enforces at least one attempt even when zero
    are configured.
  - **First priority ranking in a deliberately flat round-robin**, so it
    is a documented departure: `docs/adr/0002-priority-ranking-in-the-round-robin.md`.
  - New `CMRIHostConfig` knobs: `degradedSlotSharePercent`,
    `degradedBandwidthPercent`, `degradedBurstMs`,
    `conformanceReinitThreshold`, `conformanceReinitAttempts`,
    `breakerProbeIntervalMs`. Defaults leave a healthy-only layout
    scheduled exactly as before — with no degraded node present, neither
    gate is consulted.
  - `RemoteNodeHandle` gains `serviceClass()`, `breakerState()`,
    `conformanceBreakerOpen()`, `consecutiveNonconforming()`, and
    `breakerReinitAttempts()`. `CMRIHostStatistics` gains the
    degraded-lane ledger, carrying both denial counters separately
    because which gate bound says which failure mode is costing the
    layout. Two new events, `kBreakerTripped` and `kBreakerClosed`.
  - Tracer telemetry moves to image version `0.9.0`: host lines carry the
    ledger, node lines carry service class and breaker position.
- Observed node geometry and conformance evaluation (issue #85, Design
  v1.4 D14/D15/D16, breaking for two projected states): the Host now
  captures the geometry a Node actually demonstrates and stores a
  verdict about it, so the conformance axis stops being inert.
  `RemoteNodeHandle` gains `observedInputBytes()` and
  `lastConformanceFault()`; `CMRIHostEvent` carries the classified
  `ConformanceFault` and the expected length.
  - **This is the behaviour fix for #80.** A node declared `NI=4`
    against physically 3-byte hardware committed no data at all and
    reported `UNINITIALIZED` — "hasn't started yet" — while consuming
    63% of all poll slots. It now reports `MISCONFIGURED`, with expected
    and observed byte counts on both the handle and the event.
  - **The image axis became a validity claim, not a history claim.**
    The flag behind it recorded that a node had *once* received data,
    which is history living in the belief substrate where D15 puts only
    the current verdict. It latched, so re-init invalidation could never
    produce a "no valid image" verdict and D16's chronology was
    unimplementable. Deleted in favour of deriving validity from
    freshness; "has this node ever worked" is `statistics().exchanges`.
    Last-good bytes still survive invalidation — that hazard is about
    zeroing the buffer, not about the verdict.
  - **The projection reads the image axis three ways** under a
    nonconforming verdict: fresh gives `DEGRADED`, stale gives `STALE`,
    none gives `MISCONFIGURED`. Folding the middle case in with the last
    made `STALE`-while-nonconforming unreachable and inverted D16's
    stated order; it survived undetected because conformance was inert,
    so the branch had never executed.
  - **Only image-rung faults move the verdict.** A reply carrying
    another device's UA is definitionally not this node's, and on 2-wire
    media the Host's own poll echoes back with the polled UA and MT `P`
    — wiring that branch to the axis would park every node on every
    2-wire Host in `DEGRADED` permanently. Packet-rung faults are still
    named, classified, and reported on the event; they just do not
    change the stored verdict. This is the line the per-node `errors`
    counter already drew.
  - **No `conformanceFaults` counter.** It would duplicate per-node
    `errors` exactly, and the packet-rung total is already derivable at
    the scope that owns it: host `repliesRejected` minus the sum of
    per-node `errors`.
  - Five existing tests change verdict deliberately, including one
    pinned on #89 that recorded what the projection did *before* the
    deciding input existed. #84's recorded axis-independence criterion
    (silent liveness with a surviving image verdict) becomes
    unsatisfiable and is replaced by missing liveness with a fresh
    image, where the projection reads `ONLINE` while liveness reads
    missing — better evidence, and it needs no re-init ladder to set up.
    Amendment recorded on #84.
- Tracer node lines carry the health axes (issue #85): `liveness`,
  `imageState`, `conformance`, `observedIn`, and a `fault` block with
  the fault name, its derived layer and attribution, and expected vs
  observed. The line previously carried only the scalar projection,
  which is lossy by construction — it answers "what is the single worst
  thing about this node" — so a reader could not tell a geometry
  disagreement from any other fault. `observedIn` renders as JSON `null`
  before anything is demonstrated, because 0 is a legal geometry.
- One shared state rendering (issues #85, #93):
  `CMRInet::remoteNodeStateString()` and `remoteNodeStateTag()` live
  beside `RemoteNodeState`, with the OLED tag's three-character width
  stated as a contract. Both sketches and `TracerShell` kept their own
  copies of that switch; when `kMisconfigured` and `kDegraded` were
  added, two of the three silently began rendering `"??"` on the same
  commit, because `arduino-cli` passes no warning flags to a sketch. The
  shared helpers are compiled by every desktop test TU under `-Werror`,
  so the next enumerator breaks the build rather than the display.
- Runtime node table mutation (issue #86, Design v1.2 D5, breaking):
  the node table is now mutable at runtime within its compile-time
  capacity. `begin()` no longer locks membership — it marks the
  configuration-to-running transition and nothing more. New
  `deleteRemoteNode(address)` and
  `setRemoteNodeGeometry(address, inputBytes, outputBytes)`;
  `addRemoteNode(...)` is now legal after `begin()`. A layout can grow,
  shrink, and be rewired without restarting the Host, which is also the
  architectural prerequisite for bus discovery (#35) — auto-configuring a
  node table by polling UAs *is* runtime node addition.
  - **Capacity is not membership.** Capacity is slot availability, not a
    count that only rises, so deleting from a full table frees a slot and
    the next add succeeds. `nodeCount()` reports live nodes.
  - **Delete tombstones, never compacts**, so no surviving handle
    relocates. The slot is cleaned at delete rather than at reuse, which
    is what makes `address()` a working self-check: a handle cached
    across the delete reads back as address 0 instead of continuing to
    impersonate the node it used to serve.
  - **Address is identity**, so changing a node's address is delete +
    add. There is no in-place variant, because a cached handle would
    silently become a different logical device.
  - **Geometry change is in place and identity-preserving** — same
    address, same handle, same counters — but invalidates the cached
    input image, clears the output image, forces a re-init (I then full
    T) because the NI/NO announced in the I body changed, and degrades
    conformance to unknown since prior evidence measured a geometry that
    no longer applies.
  - **Mutating the node of the outstanding exchange is legal.** A send
    already accepted by the transport cannot be aborted (D13), so the
    exchange is *orphaned*: the frame completes, any reply is discarded,
    and nothing is attributed to any node — no node counter, no host
    reply counter, and no event either, since an event fired there would
    name a tombstone or the slot's next occupant. A packet not yet
    accepted is cancelled outright instead; nothing is on the wire to
    protect. New host-wide `CMRIHostStatistics::orphanedExchanges` keeps
    the packet ledger honest at the only scope that can own it.
  - `ConfigStatus::kAlreadyBegun` is **removed** (no mutator can return
    it now that the table is unlocked); `kNoSuchNode` added.
  - 7 new Unity tests covering the full `add -> disable -> geometry
    change -> enable -> disable -> delete -> add(reuse)` sequence,
    delete-while-in-flight asserting no attribution anywhere including
    onto the slot's new occupant, slot reuse proving a fresh node is not
    ONLINE before its first reply, and fill-to-capacity -> delete -> add.
- `onEvent()` / `onTrace()` are now legal at any time (issue #86): they
  previously refused registration after `begin()`, **silently**. That was
  coherent only while `begin()` locked the entire configuration; once D5
  unlocked the node table it left listener registration as the sole
  mutator that could fail without reporting anything, which is precisely
  the failure surface D5 forbids. Unlocking dissolves the problem rather
  than inventing a status for it, and is strictly widening — registering
  during configuration still works, so no caller changes. Swapping a
  listener between ticks is safe: the engine reads the pointer only
  inside `tick()`, and listeners already may not call back into the
  engine. `test_listener_registration_locked_after_begin` is replaced by
  `test_listener_registration_is_legal_at_runtime`, which proves a
  listener attached to a running engine receives events and that clearing
  it at runtime stops them.

### Changed
- The STALE-rung test no longer raises `missThreshold` to "prevent
  invalidation" (issue #87). That suppressed nothing: a node answering
  every poll cannot arm a silence-driven ladder at any threshold, so the
  setup implied a hazard that did not exist while the real one — the
  conformance ladder — went unnamed. It now suppresses the mechanism
  actually reachable on that path and asserts zero misses to prove which
  one that is. Same shape of drift as the D16 defect it sits next to.
- `TracerShell` no longer holds a node, and every verb names its UA
  (issue #86, breaking for the C&C vocabulary):
  - The shell bound one `RemoteNodeHandle` at `bind()` and used it for
    three different jobs — verb target, event attribution, and trace
    attribution. That is exactly the cached-handle pattern D5 deprecates,
    and it was already wrong for more than one node: every `reply`,
    `miss`, and `state` line on the dual-node bench claimed UA 30, and
    every trace line reported the bound node's address beside a packet
    that might be addressed elsewhere. Event lines now come from
    `event.node`; trace lines from `packet.ua`.
  - `bind()` drops its `RemoteNodeHandle&` parameter. Verbs take a UA:
    `quiesce <ua>`, `resume <ua>`, `forcetx <ua>`,
    `setbit <ua> <bit> <0|1>`, `writeoutputs <ua> <hex>`.
  - A UA with no live node is now a reported `noSuchNode` error rather
    than a silent no-op. After runtime delete that is an ordinary
    outcome, and a verb that quietly does nothing repeats the #82 defect
    of misreporting its own failure.
  - New D5 mutation verbs: `node delete <ua>` and
    `node geometry <ua> <in> <out>`. These exist so the interaction
    hazards can be driven against real wire timing — a mock transport
    cannot reproduce an orphan across an actual TXEN drain.
  - `node add|enable|disable` move out of `XiaoHostTracer` into the
    shared shell, so both tracer images speak one vocabulary (issue #21).
    The sketch's private copy refused `node add` after `begin()` with a
    "locked" error D5 retired, and reported success for
    `node enable|disable` on an unknown UA while doing nothing.
  - `status` splits by scope: bare `status` reports host-owned counters
    (now including `orphaned`) plus a `roster` of live nodes; `status
    <ua>` reports one node's image, health, and counters. A flat line
    mixing both was only meaningful while there was exactly one node.
  - **Telemetry speaks the UA, never the wire byte.** The old
    `"address":30,"ua":95` pair is replaced by `"ua":30`. As a side
    effect this repairs a dead check: the sketch keyed its status roster
    by `n->ua()` (the wire byte, 95) while
    `analyze_bench_validation.py` looked it up by the semantic address
    (30), so `status_state` was always `None` and its
    `UNINITIALIZED`/`OFFLINE` failure branch could never fire. See #90
    for the remaining consumers.
  - `quiesced` (a shell-wide flag that actually disabled one node) is
    replaced by per-node `enabled`.
- `XiaoHostTracer` telemetry lines are streamed rather than copied
  through a 2 KB stack buffer that silently truncated longer lines
  (issue #86). Harmless until the status line grew a roster; a truncated
  line is malformed JSON, which is worse for a runner than a slow one.
  Also returns 2 KB of stack to `loop()`. The record terminator gets the
  same room check and retry as the body, plus a reserved slice of the
  time budget: the body only ever exhausts its budget by spinning on a
  full buffer, so an unchecked terminator write would be dropped exactly
  when it matters. Under `setTxTimeoutMs(0)` a dropped newline merges two
  records and leaves the reader no boundary to resync on, which is worse
  than the truncation this replaced.
- Per-node state model split by substrate (issue #84, Design v1.3 D15/D16):
  control state, belief state, and observation state are now explicit in
  the Host path. `consecutiveMisses` moved out of `RemoteNodeStatistics`
  into control state, so observation counters stay monotonic reporting
  data. `RemoteNodeState` is now derived from stored health axes
  (`RemoteNodeLiveness`, `RemoteNodeImageState`, `RemoteNodeConformance`)
  and now includes `kMisconfigured` and `kDegraded`. `RemoteNodeHandle`
  now exposes `liveness()`, `imageState()`, `conformance()`,
  `consecutiveMisses()`, `isHealthy()`, and `inputsUsable()`. Existing
  `updateNodeStates_` behavior is now pinned by explicit projection tests.
  One reachable path is now explicit in tests: after prior good data, then
  silent invalidation, then wrong-geometry return, projection reports
  `kStale` (not `kUninitialized`) while preserving `kOffline` during the
  silent phase. Coverage added in `tests/test_host.cpp`; all test, example,
  and desktop build gates pass. Conformance is intentionally inert in
  #84 (`kUnknown`), so `isHealthy()` cannot become true yet and
  `kMisconfigured`/`kDegraded` are not reachable in this ticket.
  Issue #85 owns conformance population, reachability proofs for those
  projection states, and non-degenerate predicate divergence proofs.
  *Superseded by #85 above:* once conformance participates, that pinned
  path reports `kMisconfigured` rather than `kStale` — invalidation has
  already cleared the image, and reporting "your data is old" would
  conceal a geometry disagreement. The staging described here was
  correct for what #84 could observe; it is no longer current behaviour.
- Host configuration error model (issue #80, Design v1.2 D5, breaking):
  `addRemoteNode(...)` now returns its **own** `ConfigStatus` instead of
  `CMRIHost&`, and `begin()` returns `void` instead of a deferred status.
  `configStatus()` and the sticky host-wide status are gone. The previous
  batch model recorded the first rejection and short-circuited every later
  add so `begin()` could report it — reasonable for a hardcoded test rig,
  but wrong once configuration moved to a runtime verb shell, and a
  prerequisite for the runtime node-table mutation D5 now specifies.
  Callers registering several nodes decide for themselves how to treat a
  partial failure; the engine keeps no residual state. Chaining is retired
  (no call site actually used it except one test). All callers migrated:
  `SimpleHost`, `XiaoHostTracer`, `cmri_tracer`, `test_host`.

### Fixed
- Non-portable `printf` format specifiers across the host telemetry and
  OLED display paths (surfaced by the #93 sketch warning gate, fixed
  under #93): `uint32_t` and `unsigned long` values were passed to `%u`
  converters at ~50 sites in `src/testbed/TracerShell.h`,
  `src/SimpleHostMetrics.h`, and `examples/XiaoSniffer/XiaoSniffer.ino`.
  On the RV32 cross-compiler `uint32_t` is `long unsigned int`, so `%u`
  is a `-Wformat` mismatch; the host compiler never flagged it because
  on macOS `uint32_t` is `unsigned int`. The code compiled but was
  non-portable. Fixed by wrapping each such argument in
  `static_cast<unsigned>(...)` paired with `%u` (the tree's existing
  convention), valid on every target since `unsigned` is `unsigned int`
  everywhere. No struct field types or public APIs changed. Also removed
  an unused `using VerbResult` alias in `XiaoHostTracer.ino` flagged as
  `-Wunused-local-typedefs`.
- `XiaoHostTracer` `node add` reported `addFailed` forever after a single
  rejected add, and silently ignored every subsequent add for the life of
  the boot (issue #80). The verb inspected the sticky host-wide
  `configStatus()`, which the old batch model never cleared, and the
  engine's own short-circuit suppressed the later calls. Both are gone;
  the verb now reports the individual call's status and names the reason.

### Added
- XiaoHostTracer dual-node validation support (issue #33): generator verbs now
  accept an optional `ua` target (`configure|enable|disable ... ua <n>`),
  defaulting to UA30 when omitted for backward compatibility. Generator runtime
  state is now per-UA so `slowwalker` and `toggleoutfrominput` can run
  concurrently on multiple nodes. `toggleoutfrominput` gained explicit
  byte/bit loopback controls (`src_byte`, `src_bit`, `dst_byte`, `dst_bit`) and
  a `mode write_read` option to support loopback `write(read())` validation
  flows. Added a new non-issue-specific bench validation suite at
  `extras/bench/validation/dual_node/` with
  `gather_bench_validation.py` and `analyze_bench_validation.py` for the UA30 +
  UA31 quick validation run.
- Bench USB port discovery (issue #68): the Python bench harness no longer
  hardcodes `/dev/cu.usbmodem*` paths (they are location-derived and shuffle
  on re-enumeration). New `extras/bench/bench_ports.py` resolver keys each
  role (Host, Sniffer TX/RX, Dongle, Nodes) on the board's USB serial
  number — every Xiao ESP32-C6 exposes a unique MAC-shaped iSerialNumber —
  via `extras/bench/bench.json`, which holds explicitly named bench sets
  (`Benches` map + `Default`) so multiple benches can share one file and
  one host. The `extras/bench/bench` CLI wraps it: `list` / `check` /
  `resolve` / `import` / `export` / `seed` / `default`, with jBOM-style
  `-o console|-|file` output and `-q`/`-v` detail control. `bench import`
  enriches a human-supplied seed (flat Type/ID/USB entries) into a
  serial-keyed set, behaviorally verifying the images we own (the tracer
  answers `status`; sniffers emit image-bearing stats every 5 s); absent
  roles are carried with a null Serial and loudly reported. All gather
  scripts and `_tracer_client` resolve lazily through it; `--port`/argv
  overrides still win. 28-test functional suite
  (`extras/bench/test_bench_cli.py`) drives the real CLI against a fixture
  config plus an injected fake enumeration.

### Fixed
- Fixed a latent bug in `XiaoHostTracer` where the OLED display perpetually showed `---` and 0 for metrics because the `node` pointer was never initialized in `setup()` (issue #56).

### Added
- Host OLED diagnostics, shared across both host sketches (issue #11):
  `src/SimpleHostMetrics.h` gains a `HostStatusPanel` class that owns
  the rolling metric state (polling rate, per-node error windows) and
  formats the header and per-node row strings. Both `SimpleHost.ino`
  and `XiaoHostTracer.ino` use it — the display logic cannot drift
  between the two sketches. The header line alternates between
  cycles/sec ("XX.Xc/s") and ms/cycle ("XXXms") every 5 s, smoothed
  over a 10 s window; a stalled engine shows "---ms". Per-node rows
  show state, right-justified last-turnaround latency ("XXXms"), and a
  rolling 5-second recent-error count ("XXerr"). No new library API
  surface — all values are read from today's public `CMRIHost::statistics()`
  and `RemoteNodeHandle::statistics()`. `XiaoHostTracer` gains the
  SSD1306 OLED it previously lacked (display was #11). 11 new Unity
  tests (`tests/test_host_display_metrics.cpp`); 159 total passing.
  The per-node backoff ladder (250 ms → 32 s) is deferred to a later
  increment that needs a `RemoteNodeHandle` accessor for `pollBackoffMs_`.
- `CONTEXT.md` — project glossary defining Engine, Strategy, Shell, and
  Vocabulary precisely, so the word "engine" does not drift again. The
  key sharpening: a component that only observes or drives an engine is
  not an engine, even when it holds a reference to one.

### Changed
- `CMRITracerEngine` renamed to `TracerShell` (D1 correctness fix): the
  class was misnamed — it does not implement the image contract (not an
  Engine) and it speaks a private C&C vocabulary, not the CMRInet
  protocol (the CMRI qualifier was wrong). Renamed to `TracerShell` —
  a command-and-control shell wrapping an engine, not an engine itself.
  Mechanical rename across all three call sites (`tests/test_tracer.cpp`,
  `extras/desktop/cmri_tracer.cpp` + `Makefile`,
  `examples/XiaoHostTracer/XiaoHostTracer.ino`). Also fixes a
  pre-existing undeclared-identifier bug in `cmri_tracer.cpp` (referenced
  a `node` that was never declared).
- XiaoSniffer CDC throughput (issue #45): the sniffer was silently
  dropping ~25% of frames at high bus rates (>50 Hz) because the
  per-frame JSON line was too long for the CDC ring buffer.
  `setTxTimeoutMs(0)` made writes discard-and-return when the buffer
  was full, so bursts of frames were lost without any error signal.
  Fix: compact the frame JSON (drop the constant `image`/`version`
  fields, already in the epoch line — ~30 chars/line, 32% reduction)
  and enlarge the CDC ring buffer (`setRxBufferSize(1024)`).
  Bench-verified: capture rate improved from ~75% to 99.6% at 55 Hz.
  The OLED stays on — it was not the bottleneck.

### Fixed
- `CMRIHost` poll/transmit starvation (issue #41): `runSchedule_` could send
  a full `T` in place of a due `P` indefinitely whenever a node's outputs
  stayed dirty, so a node's own output-update cadence — or a resonance
  with another node's reply-gate timeout sharing the round-robin — could
  starve its poll/reply cycle completely (reply pair goes silent even
  though the poll pair looks healthy). Reproduced and bisected on the
  cpNode-Xiao two-board bench: confirmed independently by an RS-485
  dongle, a second passive sniffer, the Node's own TXEN LED, and an
  `onTrace` packet capture (zero `P` to the live node across the whole
  window). New `CMRIHostConfig::maxOutputPreemptMs` (default 250 ms)
  bounds how long a due poll may be deferred by a pending transmit before
  the scheduler forces it through regardless of `outputsDirty_`. That
  bound depends on the round-robin's healthy-node cycle time staying well
  under the threshold, so a companion poll-retry backoff
  (`initialPollBackoffMs`/`maxPollBackoffMs`, doubling per consecutive
  miss up to a 32 s cap, cleared immediately on any accepted reply) keeps
  a chronically offline node from taxing every pass at full priority —
  without it, the anti-starvation bound alone inverts into the
  mirror-image defect (transmit starvation). Both ship together. 3 new
  Unity tests (`test_dirty_output_cannot_starve_poll_forever`,
  `test_anti_starvation_does_not_starve_transmit`,
  `test_poll_backoff_doubles_and_clears_on_reply`); 151 total passing.
  Hardware re-verified on the same bench: `SimpleHost` (unmodified,
  previously the reliable repro) now shows `HEALTHY` with hundreds of `R`
  frames per 15 s window, confirmed A/B/A across three separate flashes.
  See `docs/sniffer-reply-pair-findings.md` for the full diagnosis record.

### Added
- `examples/SimpleHost/` — the front-door Host tutorial (issue #31):
  a behavior-only sketch a human copying this library starts from, not
  a bench instrument. Polls two nodes (UA 30 live + UA 31 phantom),
  shows per-node health on the SSD1306 OLED, and runs two behaviors: a
  1 Hz walking-one bitwalk through one output byte (visual proof the
  full T path works) and an output that toggles on each rising edge of
  a real input (the input side of the API). Table-driven node config
  (`NodeInfo` struct array), `host.node(addr)` handle lookup, and an
  `onEvent` listener that prints a rejection diagnostic over USB CDC
  ("expected N input bytes, got M") so a misconfigured geometry is
  diagnosable from the Host side without reflashing the node.
  Bench-validated on the cpNode-Xiao two-board crossover: node 30
  ONLINE, bitwalk walking, input toggle driving an output. Closes #31.
- Host public API ergonomics, surfaced and resolved by #31 (breaking):
  `addRemoteNode(...)` now returns `CMRIHost&` so calls chain
  (`host.addRemoteNode(30, 12, 12).addRemoteNode(31, 4, 4).begin()`);
  a rejected add records its reason in `ConfigStatus` and
  short-circuits later adds. `begin()` returns `ConfigStatus` (was
  `void`) — `kOk` or the reason the first add failed (`kTooManyNodes`,
  `kAddressOutOfRange`, `kAddressInUse`, `kInputBytesTooLarge`,
  `kOutputBytesTooLarge`, `kAlreadyBegun`). New flat
  `addRemoteNode(address, inputBytes, outputBytes)` overload for
  readable simple sketches. New `node(address)` accessor returns a
  registered `RemoteNodeHandle*` by address (the sketch no longer
  holds a parallel handle array). New `configStatus()` reads the
  config-phase status before or after `begin()`. All callers migrated:
  `XiaoHostTracer`, `cmri_tracer`, `test_host`, `test_tracer`.
  145 tests still pass.
- Reply-rejection diagnostics on `CMRIHostEvent` (issue #31): the
  `kReplyRejected` event now carries a `ReplyRejectReason`
  (`kUaMismatch`/`kMtMismatch`/`kGeometryMismatch`), the rejected
  reply's body `length`, `ua`, and `mt`. A conformance-checking
  sketch can now print what the remote node actually sent ("expected 7
  input bytes, got 4") without reflashing the node. The drain path
  splits UA and MT mismatch into separate reject sites so each reason
  is reported. `replyRejectReasonString()` stringifier (in-namespace,
  placeholder for a future class member).
- `configStatusString()` / `replyRejectReasonString()` —
  human-readable names for `ConfigStatus` and `ReplyRejectReason`
  (in-namespace, placeholders for future class members).
- README refreshed: new "Architecture at a glance" TL;DR condensing
  DESIGN's one-product/two-seams model, units ladder, and naming
  grammar (pointer-style to D1-D13); new "Getting started" keyed off
  `examples/SimpleHost`; stale "Planned layout" replaced with the
  real repository layout. Host-focused now; extends to both roles when
  #9 lands.
- Interop profile open questions 7 and 8 (issue #31): OQ7 — define an
  I/T acknowledgment carrying the Node's self-description so a Host
  can validate configured geometry at init time (extends the E8
  revision thread; motivated by #31's geometry-mismatch finding);
  OQ8 — bus discovery via a self-identify MT so a Host can
  auto-configure its node table. Filed as #34 and #35.
- Follow-up tickets filed from the #31 ergonomics probe: #33 (second
  physical bench node), #36 (RemoteNodeHandle card-type / IOX-offset
  awareness — the phantom-byte trap), #37 (input edge detection /
  change event), #38 (per-node listener registration or node tag).
- I/T bench slice (issue #30): the shared `CMRITracerEngine` now
  registers both D7 listeners — `onEvent` (exchange/health) and
  `onTrace` (per-packet TX/RX) — and emits a `trace` JSON line per
  packet `{dir:"tx"|"rx",mt,ua,body}`, so I and T appear in telemetry
  with their MT and body alongside the counters. New output verbs make
  T bench-exercisable: `setbit <n> <0|1>`, `writeoutputs <hex>`,
  `forcetx` (bad args emit an `error` line, not a crash). Every
  telemetry line now carries the output image as `outputs` hex
  alongside `inputs`. Desktop `cmri_tracer` gains `--output-bytes`
  (default 7); the Xiao Host sketch gains `TRACER_OUTPUT_BYTES`
  (default 7). Versions bumped: cmri_tracer 0.3.0, XiaoHostTracer 0.2.0.
  13 new Unity tests (`tests/test_tracer.cpp`); 145 total passing.
- `examples/XiaoSniffer/` — passive RS-485 bus sniffer for the testbed
  (issue #30): a spare cpNode-Xiao wires R± to one bus pair, holds TXEN
  low (listen-only), and feeds the standalone `CMRIFrameDecoder` from
  `Serial1`, emitting each decoded frame as a JSON line over USB CDC.
  No Host/transport/TX — the minimal independent witness for I/T/P
  tap acceptance, and a reusable testbed asset the software notes point
  at for the adversarial and host-conformance use cases. Direction-blind
  (reports `"observed"`); one board = one pair.
- Phase 2 `CMRIHost` — I/T sending, full-image output, and re-init
  ladder (issue #8): the engine now speaks all four polled-strategy packet
  types. Per-node on-the-wire order is I → T → P (interop 2.3.1): a
  node's first exchange is an I (CPNODE 'C' dialect, 13-byte body
  `<'C'> <dH> <dL> <opts1> <opts2> <NI> <NO> <0xFF×6>`, interop
  E3), followed by a full-image T after a 500 ms settle. T is sent on
  output change (`setOutputBit`/`setOutputs`/`forceTransmit` mark the
  image dirty) and always carries the full output image (interop 2.3.2).
  After more than 5 consecutive P misses the re-init ladder arms: the
  next slot re-sends I + full T and invalidates cached input state by
  clearing freshness only — the last-good bytes are kept, since 0 is a
  valid consumer value and zeroing would assert "all clear" (the QBASIC
  review's F15 hazard); a recovered reply disarms the ladder. dH/dL are
  per-node knobs in `RemoteNodePolicy`, default 0 (erratum E4). I and T
  expect no reply (interop E8): a packet arriving during the I settle or T
  gap is counted in `unsolicitedPackets` and discarded, not rejected or
  errored. New `CMRIHostConfig` knobs: `postInitSettleMs` (500),
  `postTxGapMs` (2), `transmitRefreshMs` (0 = off). New
  `CMRIHostEventType::kReinitScheduled` event. New `RemoteNodeHandle`
  output API: `outputBit`/`outputByte`/`outputLength`,
  `setOutputBit`/`setOutputs`/`forceTransmit`. New
  `CMRINET_HOST_MAX_OUTPUT_BYTES` geometry knob. 17 new Unity tests
  (`tests/test_host.cpp`); 132 total passing.
- `Esp32UartCMRISerialPort` promoted from the Xiao tracer sketch
  into `src/` (issue #27): hardware transmit-drain truth
  (`uart_wait_tx_done(port, 0)`) is the correct shipped behavior for
  every ESP32 target, not an R&D-only fix. Header-only,
  `ARDUINO_ARCH_ESP32`-guarded (Design v1.1 D7: a platform guard on a
  platform-specific port is not a feature toggle; non-matching builds
  see an empty file). Promotion is a relocation — the behavior is
  unchanged from the #21 wire-tap bench pass. The sketch-local copy in
  `examples/XiaoHostTracer/` is removed; the sketch's `#include` now
  resolves to the library header. Closes the sketch-local R&D fix from
  #21; #25 stays open as root cause (with #28 as its sibling).
  Promotion re-verified on the two-board crossover bench: the 0.1.3
  image ran ~4100 exchanges with all counters zero (misses/errors/
  decodeErrors/recoveries all 0, ONLINE throughout) across ~75 s, and
  the passive poll-pair tap witnessed ~4100/4100 clean `ff ff 02 5f 50
  03` poll frames — zero clipped ETX tails (the #21 `… 5f 50 ff`
  signature absent). Captures in
  `docs/bench/2026-08-16-stage2-27-promotion-host.jsonl` and
  `…-tap.hex`.
- Inter-byte abort doctrine (issue #27, Design v1.1 D13):
  `SerialCMRITransport` ships a tolerant deployment default
  (`kShippedInterByteTimeoutMs`, 100 ms) instead of the rate-derived
  three-character-time value. The reference Host lineage transmitted
  with interpreter-scale gaps (interop 2.2.6) and fielded Nodes pace
  with dH/dL (erratum E4), so a strict shipped default would fail
  conforming history; the 250 ms reply gate is the truncation
  backstop. The rate-derived value is now an explicit conformance
  instrument via `rateDerivedInterByteTimeoutMs()` (valid before
  `begin()`), never a default. Generalizes the stage-1 USB-chunking
  and stage-2 C6-stall findings: the decoder measures gaps at tick
  granularity, so any host whose tick can stall measures arrival gaps,
  not wire gaps. Be strict in what you send, forgiving in what you
  accept.
- `SerialCMRITransport::rateDerivedInterByteTimeoutMs()` — the
  conformance-strict inter-byte abort derived from the port's
  character time (three char times, interop 2.2.6). An explicit opt-in
  for tracers and conformance scenarios (Design v1.1 D13).
- `SerialCMRITransport::kShippedInterByteTimeoutMs` — the tolerant
  shipped/deployment inter-byte abort default (100 ms).
- `cmri_tracer --conformance-strict` — opt into the rate-derived
  inter-byte abort and the rate-derived slow-gap thresholds (interop
  2.2.6 conformance instrument); the USB-deployment defaults (25 ms
  abort, 1/20 ms slow-gap) remain the normal bench mode.
- `transmitDrained()` seam contract documented on `CMRISerialPort`:
  the answer may be optimistic by ignorance (a buffer-only port is the
  permitted floor) but must never be optimistic by design — never
  `true` while the port's hardware still holds untransmitted bits it
  can see. `StreamCMRISerialPort` reframed as the portable floor with
  a contribution pattern for other cores (AVR `TXCn` bit; ESP32 worked
  example in `Esp32UartCMRISerialPort`). The transport keeps the
  estimate AND port-drain conjunction even for hardware-truth ports.
- DESIGN.md bumped to v1.1: D7 platform-guard clarification,
  transport-contract estimated-drain footnote, new D13 inter-byte
  abort doctrine. Existing `Design v1.0` VALIDATION tags re-stamped to
  v1.1 (decisions D1-D12 unchanged).
- Two-threshold receive-gap observability (issue #26): the frame
  decoder now distinguishes "took longer than expected" from "took so
  long I gave up." `CMRIFrameDecoder::Statistics` carries a cumulative
  `slowGaps` counter (gaps in the suspect band [nominalHi, abort)) and a
  `maxGapMs` watermark (the largest inter-byte gap seen mid-frame,
  including the fatal abort-gap). `setSlowGapThresholdsMs(lo, hi)` sets
  the band; lo=0 disables observability, hi<=lo leaves watermark-only.
  `SerialCMRITransport` derives lo (1 char time) and hi (3 char times)
  from the port character time; the override survives `begin()`. The
  desktop tracer emits `slowGaps`/`maxGapMs` on every JSON line and the
  three thresholds on the epoch line (`--slow-gap-lo-ms`,
  `--slow-gap-hi-ms`, USB defaults 1/20). Interop profile 2.2.6 revised
  to the grace-band receive model (profile v1.1; `VALIDATION` tags
  re-stamped). This would have located the #21 2 s stall from telemetry
  alone. 12 new tests (9 codec, 3 transport); 118 total passing.
- Stage-2 bench validation (issue #21): the stage-1 scenario passed
  unchanged against the Xiao ESP32-C6 Host on the two-board crossover
  bench — smoke (UNINITIALIZED→ONLINE first reply), sustained
  2459/2459 exchanges with all counters zero (turnaround p50 7 /
  max 36 ms on the real UART), wrong-UA, unplug (OFFLINE at 6
  consecutive misses, polling continues), automatic recovery, and the
  quiesce/resume/status/quit verbs. Passive wire tap witnessed
  8124/8124 clean polls and 1499/1499 clean node replies. JSONL
  telemetry and tap captures in `docs/bench/`.
- `src/testbed/CMRITracerEngine.h` — the shared testbed C&C engine
  (verbs in, JSON-lines telemetry out, D7 listeners), wrapped by both
  the desktop `cmri_tracer` and the Xiao Host sketch: "same engine,
  same listeners, different main()" is now enforced by construction.
  `ts` is integer ms since the stream's epoch line in every image;
  the epoch line carries the absolute anchor (`wallClock` on the
  desktop, `bootMs` on the Xiao) (issue #21).
- `examples/XiaoHostTracer/` — the stage-2 Xiao Host R&D image:
  CMRIHost on the cpNode-Xiao RS-485 block (28800 8N2, RX=D7, TX=D6,
  TXEN=D3), C&C over USB CDC, no OLED/OTA/WiFi. Build knobs
  `TRACER_ADDRESS` / `TRACER_INPUT_BYTES` / `TRACER_BAUD` /
  `TRACER_INTER_BYTE_TIMEOUT_MS` via `build.defines` (issue #21).
- `Esp32UartCMRISerialPort` (sketch-local): transmitDrained() from
  `uart_wait_tx_done(port, 0)` — hardware TX-complete truth instead
  of the buffer-level answer plus wire-time estimate (issue #21).
- Stage-1 bench validation (issue #7): the P/R tracer bullet ran on a
  real wire — Mac desktop Host through a USB-RS485 adapter to the
  flashed cpNode-Xiao (UA 95, 28800 8N2). Sustained run 1000/1000
  exchanges with 0 misses and 0 decode errors (turnaround p50 23 /
  max 41 ms); wrong-UA, unplug (OFFLINE, polling continues), and
  reconnect (automatic recovery) negative tests all passed. JSONL
  telemetry captures in `docs/bench/`.
- `CMRIHost::onEvent()` / `onTrace()` (D7 listener seam) —
  function-pointer listeners with a context cookie, locked at
  `begin()`: engine events (reply accepted/rejected, miss, node state
  change with old/new state) and TX/RX packet traces. 6 new tests
  (issue #7).
- Desktop Host harness (`extras/desktop/`, outside the Arduino
  build): `PosixCMRISerialPort` — the byte-port seam over fully raw
  termios (no IXON/IXOFF, per review-CMRI-Controller-host.md Finding
  7), non-blocking, 8N2, macOS `IOSSIOSPEED` for the nonstandard
  fielded 28800 rate — and `cmri_tracer`, a command-and-control CLI
  around `CMRIHost`: verbs (`quiesce`/`resume`/`status`/`quit`) on
  stdin, JSON-lines telemetry (monotonic seq, epoch marker,
  cumulative counters) on stdout (issue #7).
- Bench finding: USB serial adapters deliver RX bytes in
  latency-timer chunks (FTDI default 16 ms), so arrival gaps are not
  wire gaps; the rate-derived inter-byte timeout (~2 ms at 28800)
  aborts healthy frames. The tracer defaults the override to 25 ms
  (`--inter-byte-timeout-ms`).
- `CMRInet::CMRIHost` (`src/CMRIHost.h/.cpp`) — the polled-strategy
  Host engine, P/R slice: non-blocking `tick(nowMs)` poll schedule,
  round-robin over enabled nodes, reply gate opened at `sendComplete()`
  (default 250 ms, per-node override via nested `RemoteNodePolicy`),
  UA/MT reply verification, geometry-checked commit, miss counting
  with recovery and OFFLINE detection, poll pacing, and cumulative
  `CMRIHostStatistics` (issue #6).
- `CMRInet::RemoteNodeHandle`, `RemoteNodeConfig`, `RemoteNodeState`,
  `RemoteNodeStatistics` (`src/RemoteNodeHandle.h`) — the
  strategy-neutral per-node product surface: input image reads,
  freshness age, health state, cumulative statistics, and
  disable-only lifecycle. New `CMRINET_HOST_MAX_INPUT_BYTES` and
  `CMRINET_HOST_MAX_NODES` geometry knobs.
- 20 new Unity tests (`tests/test_host.cpp`) against the mock
  transport and scripted replay rig, covering poll emission, reply
  gating and overrides, reply verification, miss/recovery/offline
  and staleness transitions, pacing, backpressure retry, and
  byte-level gapped replay through the real decoder (issue #6).
- `CMRInet::SerialCMRITransport` (`src/SerialCMRITransport.h/.cpp`) —
  the serial/RS-485 packet transport: codec integration, non-blocking
  TXEN discipline (assert, gapless write, flush to full drain via
  wire-time estimate AND port drain report, deassert at once),
  rate-derived default inter-byte timeout (overridable, 0 = tolerate
  any gap), and decoder + UART error folding into `stats()`
  (issue #5).
- `CMRInet::CMRISerialPort` (`src/CMRISerialPort.h`) — the byte-port
  seam under the serial transport (raw bytes, TXEN line, drain query,
  character duration, hardware error count), so desktop tests drive
  the exact TXEN/drain/timeout discipline that runs on hardware.
- `CMRInet::StreamCMRISerialPort` (`src/StreamCMRISerialPort.h`,
  Arduino-only) — adapter over an Arduino `Stream` plus optional TXEN
  pin; stop-bit hook is the sketch's `Serial.begin(baud, SERIAL_8N2)`
  plus the adapter's bits-per-character (default 8N2).
- 14 new Unity tests (`tests/test_serial_transport.cpp`) against a
  scriptable fake port, covering TXEN ordering, `sendComplete`
  semantics, chunked writes, receive-during-drain, and stats.
- `CMRInet::CMRITransport` (`src/CMRITransport.h`) — the packet-seam
  transport contract with `LinkStatistics`, per the DESIGN.md transport
  contract (issue #4).
- `CMRInet::MockCMRITransport` — deterministic mock transport and
  scripted replay rig: packet- and byte-level injection (bytes run
  through the real decoder, optionally gap-metered per byte), send
  latency/backpressure/link-down shaping, sent-packet observation, and
  an ordered (UA, MT)-matched reply script (packet, bytes, or silence;
  repeat counts). Fixed-capacity storage, no allocation (issue #4).
- `CMRInet::Deadline` and `CMRInet::Age` (`src/CMRITime.h`) —
  injected-time helpers adapted from the elapsedMillis idea:
  centralized wrap-safe clock math with explicit armed/marked state,
  fail-safe sticky-due latching, and saturating ages with named
  `kNeverMarked` / `kExceededCapacity` sentinels.
- `// VALIDATION:` comment convention
  (`docs/agents/validation-comments.md`): greppable, versioned
  provenance tags from code to Spec / Interop / Design clauses;
  `Version:` lines added to DESIGN.md and the interop profile.
- 37 new Unity tests (`tests/test_time.cpp`,
  `tests/test_mock_transport.cpp`); the tests Makefile now builds
  multiple suites.
- `CMRInet::CMRIPacket` — the direction-neutral packet value type
  `{UA, MT, body}` with a `CMRINET_MAX_BODY` geometry knob (issue #3).
- Serial codec (`src/CMRIFrameCodec.h/.cpp`): `encodeFrame()` and the
  resumable non-blocking `CMRIFrameDecoder`, implementing the framing and
  DLE-escaping rules of the interop profile Part 2 (two-SYN preamble,
  escape 0x02/0x03/0x10 in every body including I, never 0xFF, DLE-aware
  hunt, inter-byte timeout, commit-on-ETX staging).
- Desktop test harness: vendored Unity v2.6.1 (no Ceedling) with a plain
  Makefile (`make -C tests`); 29 byte-vector tests seeded from the
  `docs/research/comparison.md` §3 anti-checklist.

### Fixed
- `examples/XiaoSniffer/` no longer stalls when a USB cable is plugged
  in but no terminal is open (DTR not asserted): `Serial.setTxTimeoutMs(0)`
  in `setup()` makes CDC writes discard-and-return when no host is
  draining the ring buffer, instead of blocking ~100 ms per write
  (Espressif's documented HWCDC workaround, arduino-esp32 #9043; safe on
  the C6's HWCDC path). At many frames/s that per-write stall was
  starving `loop()` and freezing the OLED.
- `examples/XiaoSniffer/` no longer hangs on a headless board: the
  USB CDC host wait in `setup()` is bounded at 3 s, and `emitLine()`
  skips the blocking `Serial.write` when no terminal is attached
  (live check; a later-attaching terminal resumes output). The OLED
  dashboard also no longer garbles once per-MT counters grow: the
  single `I:%u T:%u P:%u R:%u` line exceeded the 21-char width and
  wrapped onto the row below, so `drawStatus()` is reflowed to a fixed
  5-line layout (frames / P+R / T+I / abort+rst / last ua), every line
  <= 21 chars, with `setTextWrap(false)` as a belt-and-suspenders clip
  guard. Version bumped 0.2.0 -> 0.2.1.
- `CMRIHost.h` now includes `Arduino.h` for the ARDUINO-only
  `tick()` convenience overload — the library's first real Arduino
  compile could not see `millis()` (issue #21).
- TXEN clipped the poll's ETX mid-air every ~2 s on the Xiao Host
  (wire-tap verified `… 5f 50 ff` tails): the ESP32-C6 Arduino
  runtime stalls ~25–35 ms about every 2 s, the drain estimate
  expired during the stall, and TXEN dropped while the UART was
  still shifting. Fixed by hardware drain truth
  (`Esp32UartCMRISerialPort`); receiver-dependent symptom — the
  FTDI tap decoded some clipped tails clean while the node's
  MAX3491 saw garbage (issue #21).

### Changed
- `examples/XiaoSniffer/` OLED counters now print in fixed-width,
  right-aligned fields (`%10u`/`%8u`/`%4u`/`%3u`) so the labels and
  the second counter on each line stay pinned as digit counts change —
  no layout shift when a value grows by a digit. Pure UX polish on the
  0.2.1 fixed 5-line dashboard; no version bump.
- `examples/XiaoSniffer/` OLED right-column counters (R, I, rst) now
  share the same right margin so their digits line up vertically: I
  widened to `%8u` and rst to `%6u`, both ending at column 21 like R.
  Left-column counters (P, T, abort) keep column 10.
- Xiao Host R&D image version bumped to 0.1.3 (issue #27): the
  `Esp32UartCMRISerialPort` include now resolves to the library header
  and the port usage is namespace-qualified (`CMRInet::`). The image
  keeps its explicit 50 ms inter-byte override, so runtime behavior is
  unchanged from 0.1.2.
- Inter-byte timeout doctrine, generalizing the stage-1 USB-chunking
  finding: the decoder measures byte gaps at tick granularity, so
  ANY host whose tick can stall (USB latency timers, runtime
  housekeeping) measures arrival gaps, not wire gaps. The rate-derived
  default is a conformance instrument; deployed Hosts should run a
  tolerant limit (Xiao image: 50 ms) and rely on the 250 ms reply
  gate as the truncation guard. Be strict in what you send,
  forgiving in what you accept (issue #21).
