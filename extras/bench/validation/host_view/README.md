# Host-view validation scenarios (UA30/UA31 real nodes, optional UA32 phantom)
Quick physical-bench scenarios focused on Host-declared topology and semantic-status outcomes.

## Purpose
- Validate that Host-declared geometry is scored from semantic status/evidence, not just wire activity.
- Validate expected offline behavior for an explicitly declared nonexistent node (UA32).
- Keep scenario setup and scoring in one reusable gather/analyze pair.

## Scenarios
- `real-nodes-pass`
  - Declares UA30 + UA31 with expected geometry and expects both `online`.
- `real-nodes-mismatch`
  - Declares UA30 + UA31, but intentionally misdeclares one real node's geometry.
  - Expects `geometry_mismatch` for the targeted node.
- `real-nodes-plus-ua32`
  - Declares UA30 + UA31 (real) and UA32 (nonexistent).
  - Expects UA30 + UA31 `online`, UA32 `offline`.

## Run
```shell
extras/bench/.venv/bin/python extras/bench/validation/host_view/gather_host_view_validation.py --scenario real-nodes-pass
```

Then analyze:
```shell
extras/bench/.venv/bin/python extras/bench/validation/host_view/analyze_host_view_validation.py extras/bench/validation/host_view/data/results.YYYYMMDD.host_view
```

## Notes
- The gather script writes:
  - capture log (`<tag>.log`)
  - manifest (`manifest.json`) with per-UA expectations, status snapshot, and per-node status payloads.
- The analyzer treats scenario expectations as the oracle:
  - `online`: accepted exchanges, replies, healthy state, no geometry disagreement.
  - `geometry_mismatch`: mismatch fault/state evidence is present.
  - `offline`: no accepted exchanges/replies and offline/uninitialized state evidence.
