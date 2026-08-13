# CMRInet testbed software notes
These notes capture the current testbed thinking. They are project-private and experimental. They are not a protocol specification.

Keep the first working version simple. The software side of the testbed lets a person or an agent run bounded bench scenarios and get useful diagnostics. It does not require WiFi, a dashboard, or production firmware support during the tracer effort.

Companion document: [testbed-physical-notes.md](testbed-physical-notes.md) covers boards, cables, and adapters.

## Use cases
Four testbed products share one substrate. Each differs in what is under test and in verdict shape. Three drive from the Host seat. The fourth (Host conformance) inverts the seat: the scripted Node is the tester.

### Production hardware test
The item under test is a physical board or set of boards.

The runner gets configuration guidance and tells the operator how to wire the bench. A scenario can include manual steps, such as wiring card 0 outputs to card 1 inputs.

The verdict shape is GREEN OK or RED FAIL. A failure must name the failed card, bit, connector path, or setup step when possible.

### Adversarial conformance suite
The item under test is Node firmware, such as cpNode, cpCMRI, or ArduinoCMRI.

The runner exercises edge cases and per-spec behavior. The cases come from the NMRA spec, the interop profile, and the recurring-bug anti-checklist in `research/comparison.md` §3.

The verdict shape is one result per case. A failure cites the spec clause, erratum, or interop-profile rule that the firmware did not meet.

This use case needs a warty-Host capability: scripted adversarial sends (gapped bytes, truncations, bad escapes) from the desktop Host. That is a small extension of the tracer Host, not a new machine.

### Host conformance suite (deferred)
The item under test is foreign Host software, such as JMRI.

A passive node proves little here, because the Host owns the schedule. The tester is an active scripted Node: it classifies observed Host traffic against the interop profile, and it provokes Host reactions with controlled replies (late, overlong, mis-escaped, truncated, silent-then-recovered). That is the warty `CMRINode` from map issue #10, pointed at a foreign Host.

JMRI needs no bench hardware for this: JMRI speaks CMRI-over-TCP, so a desktop `CMRINode` over a TCP transport is a pure software rig.

The verdict shape is per interop-profile rule: observed-conformant, observed-nonconformant with a citation, or could-not-provoke. Results feed the errata paper (#15) as empirical evidence alongside the code inspections.

Sequenced after issue #10. Until then the adversarial code inspections in `research/` cover this ground.

### CMRInet functional tests
The item under test is this library.

Most tests run with mocks and POSIX builds on macOS, Windows, and Linux. The hardware slice exists only for the limits and timing behavior that mocks cannot show.

The verdict shape belongs to the test framework. The hardware smoke subset reuses the same scenario verbs and telemetry as the other flows.

## Shared substrate
The shared substrate is a Host-driven scenario layer: a Host that runs scripted scenarios against a real Node, observes replies and link events, and emits machine-readable telemetry.

Verbs and telemetry live in the substrate. Verdict logic lives in a runner, one runner per use case, written when that use case is real. No verdict shape gets frozen into firmware or protocol.

## Likely sequence
1. Desktop `CMRIHost` is the Host. A Mac plus a USB-RS485 adapter drives a physical Node. The command-and-control stream is process stdin/stdout. This needs no new firmware surface — it arrives with the tracer bullet (map issues #3, #5, #6).
2. Xiao Host R&D image. The same verbs run over USB CDC. If the verbs map onto the `CMRIHost` public API plus its `onEvent`/`onTrace` listeners and nothing else, the desktop binary and the Xiao sketch are the same command-and-control engine around the same protocol engine. A scenario that passes against the desktop Host must pass against the Xiao Host.
3. Xiao Node R&D image. Node-specific verbs (set inputs, select warty profile) over USB CDC, same grammar.
4. Standalone testbed, much later. The Xiao Host runs a scenario table stored on the board. The verb engine stays, the driver changes.

Not every firmware image needs every capability. The command-and-control code can exist only in an R&D test image and be absent from user production builds.

## Command-and-control seed
Do not formalize this early. It begins project-private, focused on tracer testing. Plain verbs in, JSON lines out. Human-typable, machine-parseable.

Stream shape rules that cost nothing now and preserve options later:
- cumulative counters, never deltas
- monotonic sequence numbers
- an explicit epoch marker on reset, so a runner detects board resets

Useful early telemetry fields: role, image name and version, UA, link state, poll count, reply count, miss count, framing-error count, last input image, last output image, local timestamp.

The first bus-control verb is `quiesce` (with `resume`). It becomes necessary the day a desktop Host and a Xiao Host can both attach to the same half-duplex pair. Before then, no verbs are required at all.

## Deferred
- WiFi command-and-control and TCP console by mDNS hostname (after the tracer effort, maybe well after)
- USB port registry
- on-board verdict generation
- formal bench protocol document
- standalone conformance-test sketch

## Map implication
No new ticket yet. These ideas stay as fog notes until the tracer bullet produces a working Host, transport, and bench loop. The first real ticket appears when the bench needs a concrete harness feature to validate a frontier issue.
