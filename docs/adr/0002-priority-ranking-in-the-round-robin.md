# ADR-0002: A priority ranking enters the flat round-robin
Date: 2026-08-26
Status: Accepted
Related: #87, #80, #41
Cross-links: `docs/DESIGN.md` D16, D17
## Context
The exchange schedule has been a deliberately flat round-robin since the first tracer bullet. Every enabled node got a turn in slot order, and the only per-node gate was the poll-retry backoff added for #41. Flatness was a feature: it made cycle time predictable, made the trace readable, and left nothing to tune.
Flatness assumed every node is worth a turn. The #80 bench disproved that. A node declared `NI=4` answering with 3 bytes took 1316 polls to a healthy node's 765 — 63% of all slots, serviced 1.7x more often than the node doing real work, while committing nothing. A geometry mismatch clears the poll backoff (correctly: the reply proves the node is alive), so the broken node was immediately eligible on every rotation while the healthy one occasionally had other work queued.
It was not merely adding traffic. It was receiving preferential service.
## Decision
Two service classes, healthy and degraded, with different guardrails. This is the first priority ranking in the schedule and is recorded here because it retires a property the design previously valued.
The degraded lane is admitted through two gates that must both pass — rotation slots and wall-clock bandwidth — with a ceiling clamp beneath them that guarantees the class is never starved to zero. `docs/DESIGN.md` D17 holds the normative mechanism.
## What flatness bought, and what replaces it
Predictable cycle time. Flatness gave one bound for the whole bus; the gates give a stronger one, because a healthy node's cycle time is now bounded independently of how many broken nodes share the rotation. Under flatness a dozen rotting nodes degraded every healthy node without limit — which is precisely the forcing scenario in #87.
A readable trace. This is a real loss. Degraded traffic no longer appears at a fixed cadence, so a reader cannot infer the rotation from the trace alone. The degraded-lane ledger in the telemetry (grants, per-gate denials, clamp bypasses) exists to replace what the trace used to show implicitly.
Nothing to tune. Also a real loss: `degradedSlotSharePercent` and `degradedBandwidthPercent` are two new knobs where there were none. Mitigated by making them inert in the common case — a layout with no degraded node consults neither gate and schedules exactly as before.
## The scope limit that keeps this honest
The gates are consulted **only** when a healthy node is contending. The justification for ranking is protecting healthy nodes; where there are none, ranking protects nothing and only delays recovery. Without this limit a lone silent node jumps from the 250 ms backoff ladder straight to the ceiling clamp on its first miss, pushing interop 2.3.10's re-init ladder from about 16 seconds out past three minutes.
So the ranking is not "degraded nodes are worth less". It is "degraded nodes do not get to displace healthy work". Those differ exactly when there is no healthy work, and the second is the one the evidence supports.
## Alternatives rejected
A single gate. Rejected on measurement, not taste. A silent probe costs a full reply timeout (order 250 ms) and one slot; an answering nonconforming probe costs turnaround (order 15-20 ms) and one slot. A wall-clock budget barely notices the second; a slot budget barely notices the first. One 60 s capture holds both classes on one bus at 7 polls against 1206 — a 172x gap that no single currency measures.
Per-node budgets instead of an aggregate. Rejected because organic rot produces several broken nodes at once. Per-node bounds let their sum grow without limit, and the sum is what a healthy node experiences.
Leaving it to the poll-retry backoff. That is what the code already did. It works for silence and does nothing for nonconformance, because a mismatch clears it.
Dropping the nonconforming node from the rotation entirely. Rejected: zero traffic means no evidence of recovery can ever arrive, so a reflashed node could never come back without restarting the Host.
## Consequences
The scheduler now has state that is not per node: two host-wide accumulators. They are control substrate (D15) and are reset only by construction.
`RemoteNodeState` gains no enumerator — service class is a separate derived reading, because "how is this node scheduled" and "what is wrong with this node" are different questions and collapsing them would put a scheduling concern in the product-facing health projection.
Bench acceptance for the fairness claim needs the generator-active scenario. Existing `host_view` captures run no generators, so nothing competes for slots and preferential service cannot appear in them at all — in the very capture that motivates this ADR, the healthy and nonconforming nodes poll near-evenly (1240 against 1206) for exactly that reason.
## Revisit trigger
Revisit if a third service class is ever proposed. Two classes are defensible as "protected" and "not allowed to displace the protected"; a third would need its own evidence, and the flatness argument above should be re-read before adding one.
