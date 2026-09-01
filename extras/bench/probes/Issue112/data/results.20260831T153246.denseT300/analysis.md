# denseT300 capture analysis (#112)

Stamp: `results.20260831T153246.denseT300`

Host: XiaoHostTracer **0.10.0**, dual node UA30 (7/7) + UA32 (4/4 phantom),
fastwalker **300 ms** on both UAs, 60 s `run`. 4-wire dual sniffer (TX/RX).

## Headline
| surface | value |
|---------|-------|
| polls (status_post) | 2269 |
| replies | 2219 |
| rejected | 0 |
| unsolicited | **0** |
| live miss | **42** |
| live reply (during filtered run) | 40 |
| live xchg | 2350 |
| ring dump packets | 4394 |

Miss rate ≈ 42/2269 ≈ **1.85%**. UA30 owns 38 misses; UA32 owns 4 (silent phantom ladder).

## Instrumentation health (package B)
- Every miss: `kind=P`, `outcome=timeout`, `gateMs` 250–252 (full reply gate, not early abort).
- Transport snapshot present on all misses (`echo=auto`, cumulative `rxDuringTx=1`).
- xchg outcomes: accepted 2124, delivered 183, timeout 42, settled 1.
- `run` kept miss/xchg live (not silenced). Packet traces still ring-only during run.

Happy-path replies still show **gateMs ≈ 6–8 ms** turnaround when accepted.

## Decision table (Host ring R vs miss.gateArmedMs)

Ring PKT `t=` and `gateArmedMs` share Host `millis()`.

| class | count | meaning |
|-------|------:|---------|
| R inside gate, Host miss | **0** | not "R on wire, Host ignored it" |
| R between miss and next P | **0** | would have been `unsolicited` |
| R after next P | **38** | late answer consumed by *next* poll |
| no R after arm (UA32) | **4** | true silence on phantom |

First R after a UA30 miss lands **~260–280 ms after gate expiry**, almost exactly when the next P for that UA goes out — ring shows `nextP` then R ~10–15 ms later (normal turnaround on the *following* poll).

So this capture's Host misses are **full-gate timeouts with no in-window R**, not RX-path rejects and not unsolicited late R. Dense full-T still correlates as **load** (183 T delivers; miss `prevKind` almost always P).

## Sniffer structural (4-wire)
- TX sniffer: P=2269 R=2261 T=197 (TX pair also hears some R bleed depending on wiring).
- RX sniffer: P=2269 R=2219 T=197.
- Semantic UA30: sniffer P−R delta ≈ **+42**, matching live miss count.
- UA32: P small, R=0 — phantom offline as expected.

## What this rules down
1. Not "R in gate + Host miss only" (verify/RX drop of a present R) on this 4-wire run.
2. Not unsolicited late-R (Host moved on while R in flight outside gate) — count 0.
3. Misses are real empty gates; recovery is the next poll's normal ~7 ms R.
4. UA32 misses are genuine silence (no ring R).

## Still open
- **A.** 2-wire one sniffer clock (cross-clock lock; this run is dual-sniffer 4-wire).
- **C.** `postTxGapMs` / echo AlwaysOn|Off under dense T.
- Mechanism of "empty 250 ms gate then next poll succeeds in 7 ms" under T load (schedule/backoff/node readiness), not yet a single-stage root cause.

## Artifacts
`extras/bench/probes/Issue47/data/results.20260831T153246.denseT300/`
- `host_live.jsonl` — live miss/xchg/reply/backoff during run
- `packets.Host.raw` / `TX.raw` / `RX.raw`
- `ring.dump.log` — Host packet ring
- `status_pre.json` / `status_post.json` / `manifest.json`
