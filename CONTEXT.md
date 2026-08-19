# CMRInet — Context (Glossary)

This file holds the project's ubiquitous language: the terms that name
domain concepts, defined precisely so names in issues, code, and docs do
not drift. Conceptual only — no implementation details. Decisions that
record *why* a term is defined this way live in `docs/adr/`.

## Engine

A component that implements the **image contract** (bits, freshness,
health — the sketch-facing surface) by some exchange discipline; the
implementation of a **Strategy**. Named `[protocol][role]`:
`CMRIHost`, `CMRINode`.

A component that only *observes* or *drives* an engine — telemetry
rendering, command-and-control dispatch, status display — does **not**
implement the image contract and is therefore **not an engine**, even
when it holds a reference to one. Naming such a component `*Engine`
claims a role it does not play.

## Strategy

An exchange discipline that fulfills the image contract. Polled
CMRInet is the first strategy. A component that does not speak the
CMRInet protocol must not carry the `CMRI` qualifier (it is not a CMRI
strategy).

## Shell

A command-and-control wrapper around an **Engine**: it parses a
**Vocabulary** of verbs, renders telemetry, and bridges the engine's
observability seam (event/trace listeners) to a command-and-control
stream. A shell drives an engine; it does not implement the image
contract. Testbed-only; lives in `CMRInet::testbed`.

## Vocabulary

The verb set a **Shell** dispatches — for the tracer shell,
`quiesce | resume | status | setbit <n> <0|1> | writeoutputs <hex> |
forcetx | quit`. A property of a shell, not a component in its own
right.
