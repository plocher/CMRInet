# ADR-0001: UA vocabulary and layering boundary for node vs packet surfaces
Date: 2026-08-25
Status: Deferred
Related: #90, #9
Cross-links: `docs/DESIGN.md` D1, D11, D12, D14
## Context
The current issue is not only naming drift. It is a boundary leak between product-layer node identity and strategy-specific packet encoding.
`RemoteNodeHandle` is a strategy-neutral product type. Its `ua()` accessor currently exposes the CMRI/serial encoded byte (`address + 65`), which is a transport-level concept.
At the same time, tooling has mixed vocabularies: map keys created from one representation and read with the other.
This produced silent mis-attribution in bench validation and probe analyzers.
## Decision
For issue #90 work, contracts are split by surface and must not be mixed:
- Node-scoped surfaces (`status`, roster, node-health/event consumers) use semantic UA only (`0..127`).
- Packet/trace surfaces also use semantic UA in decoded telemetry lines.
- Any truly raw-byte view remains raw and may include encoded wire bytes; raw views are not decoded telemetry.
- Inputs to tooling and CLI verbs are semantic UA only.
- Mixed-vocabulary fallback is rejected as unsound.
This ADR does not change `docs/DESIGN.md` normative clauses yet.
`docs/DESIGN.md` remains the contract source. This ADR records rationale and deferred resolution for the product API layering question.
## Why fallback is unsound
Node addresses are `0..127`; encoded wire bytes are `address + 65` (`65..192`).
The overlap (`65..127`) is ambiguous by value alone (for example `96` may be semantic UA `96` or wire encoding of UA `31`).
Therefore compatibility heuristics cannot be made correct.
## Deferred API question
Whether `RemoteNodeHandle::ua()` should be removed, renamed, or redefined is deferred.
That decision must be made with the role/strategy design work (`#9`) and its MQTT-path implications (D11/D12), not only from the serial carrier viewpoint.
Interim stance:
- Do not widen product-surface dependence on encoded wire identity.
- Keep encoded-wire concerns attached to packet-level representations.
## Consequences
Immediate:
- Tooling and telemetry contracts can be made unambiguous in #90.
- Bench acceptance logic in #88 is less likely to repeat address-vocabulary false positives.
Deferred:
- Product API cleanup remains open until #9 settles cross-role and cross-strategy vocabulary at once.
## Revisit trigger
Revisit this ADR when #9 enters active design/implementation.
At that point, either:
- promote final contract language into `docs/DESIGN.md` and mark this ADR Superseded, or
- close this ADR as Accepted with explicit DESIGN clause updates.
