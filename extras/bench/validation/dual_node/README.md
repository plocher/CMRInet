# Dual-node bench validation (UA30 + UA31)
Quick bench validation tooling for the second physical cpNode bench node work (#33).

## Purpose
- Run one focused capture with traffic on both nodes.
- Verify both nodes initialize and respond to polls.
- Leave a clear manual visual check step for both bitwalkers.

## Traffic profile
For both UA30 and UA31, the gather script configures:
- `slowwalker` on output byte 3 for UA30 and byte 2 for UA31
- `toggleoutfrominput` in `write_read` mode with explicit `src_byte/src_bit` and `dst_byte/dst_bit`
  - UA30 default: byte 3 bit 1
  - UA31 default: byte 2 bit 1

Override jumper mapping as needed with:
- `--ua-a-loopback-byte/--ua-a-loopback-bit`
- `--ua-b-loopback-byte/--ua-b-loopback-bit`

This uses the new UA-aware generator syntax:
- `configure slowwalker ua <ua> ...`
- `configure toggleoutfrominput ua <ua> mode write_read src_byte <n> src_bit <n> dst_byte <n> dst_bit <n>`

## Run
```shell
extras/bench/.venv/bin/python extras/bench/validation/dual_node/gather_bench_validation.py
```

Then analyze:
```shell
extras/bench/.venv/bin/python extras/bench/validation/dual_node/analyze_bench_validation.py extras/bench/validation/dual_node/data/results.YYYYMMDD.bench_validation
```

## Success criteria
Automated:
- UA30 and UA31 both show TX init traffic (`I`/`T`)
- UA30 and UA31 both show TX polls (`P`)
- UA30 and UA31 both show RX replies (`R`)

Manual:
- Visual exam confirms both node bitwalkers are operating.
