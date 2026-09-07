# CMRInet — agent configuration

## Agent skills

### Issue tracker

Issues live in this repo's GitHub Issues (`plocher/CMRInet`, via the `gh` CLI). See `docs/agents/issue-tracker.md`.

### Triage labels

Default vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `CONTEXT.md` and `docs/adr/` at the repo root (created lazily). See `docs/agents/domain.md`.

### Validation tags

Code links to design and spec clauses with greppable `// VALIDATION:` comments that cite identifiers, never file paths. See `docs/agents/validation-comments.md` for the grammar, the identifier-to-file mapping, and the re-verification workflow.

## Project orientation

- User guides: `README.md` is the front door; the `README-*` guides beside it (install, hardware, tutorials, api, protocol, references) are the new-user task tier.
- Architecture and decisions: `docs/DESIGN.md` (D1-D17; read before any implementation work).
- Wire behavior: `docs/cmrinet-interop-profile-and-errata.md` (normative for this library).
- Evidence base: `docs/research/` (seven adversarial reviews + `comparison.md` synthesis).
- Terminology: Host and Node per LCS-9.10.1 (DESIGN.md D1). "CMRInet" is one word, `net` lowercase.
- Bench tooling: `extras/bench/README.md` (setup, device roles, capture and verdict scripts).
- Compile gates: `make check` runs the desktop unit tests, the desktop tracer build, and the example-sketch warning gate (`docs/sketch-warning-gate.md`).
