# QBASIC CTC Machine (Chubb SPS lineage) — CMRInet Host-side review

## Scope & sources

Reviewed implementation: **Southern California Railway CTC Machine**, a QuickBASIC 4.5
DOS program whose serial protocol core is Bruce Chubb's own Serial Protocol
Subroutines package (`SPSQBC01.BAS`, dated June 30 2003 — see the header block at
`RR1-46.BAS.txt:747-753`). This is the closest surviving lineage to the reference
C/MRI Host: the spec's own normative reference [1] is the 2013 edition of these
same subroutines. Its wire behavior is, for practical purposes, *the original
de-facto standard that classic Nodes were tuned against.*

Sources:
- `/Users/jplocher/Dropbox/workspace/C-MRI Code V.1-44/RR1-46.BAS.txt` (v1.46, 4474 lines) — primary.
- `/Users/jplocher/Dropbox/workspace/C-MRI Code V.1-44/RR1-44.txt` (v1.44, 4415 lines) — compared.
- Spec: `docs/lcs-9.10.1_cmrinet_v1.1.pdf` (NMRA LCS-9.10.1 v1.1, cited by page).
- Prior research: `docs/research/comparison.md` §1–§2 (built upon, not repeated).

**Version delta (v1.44 vs v1.46):** the five protocol subroutines (`INIT`,
`INPUTS`, `OUTPUTS`, `RXBYTE`, `TXPACK`) and the node-configuration subroutine
`INITRR` are **byte-for-byte identical** in both files (verified by extraction
and diff). The poll order and per-node T dispatch are also identical
(`RR1-44.txt:1671-1807, 4163-4413` vs `RR1-46.BAS.txt:1730-1865, 4222-4472`).
All v1.45/v1.46 changes are CTC application logic (signal aspects, direction of
travel — changelog at `RR1-46.BAS.txt:32-34`). All citations below are to
`RR1-46.BAS.txt`; note the source has no numeric BASIC line numbers (QB4.5
labels/SUBs), so citations are SUB/label name + file line.

## Architecture summary

A single-threaded, blocking, synchronous Host. Main loop
(`RR1-46.BAS.txt:298-342`): `readrr` (poll all 5 nodes, blocking) → CTC
application logic → `writerr` (send T to all 5 nodes). No interrupts, no OS
serial driver — the UART (8250/16450/16550) is programmed and polled directly
via `INP`/`OUT` on the COM-port base address (COM1=0x3F8 … COM4=0x2E8, decimal
at `INIT`, lines 815-818).

Protocol layer = five subroutines (documented at lines 754-765):
- `INIT` — validates node parameters, programs the UART, builds and sends the I message (746-1036).
- `INPUTS` — sends P, then linearly parses the R reply into `ib(1..NI)` (1295-1345).
- `OUTPUTS` — sends T from `ob(1..NO)` (1699-1711).
- `RXBYTE` — polled single-byte receive with an iteration-count timeout (2090-2144).
- `TXPACK` — frames and transmits one message, byte-at-a-time, THR-polled (3821-3869).

Node roster (`INITRR`, 1047-1126): four SMINI nodes (UA 0-3, layout towns) and
one SUSIC 32-bit node (UA 4, the CTC machine itself: NS=4, CT = 102,106,106,1,
NI=20, NO=32). Global parameters: COM1, 9600 baud, DL=0, MAXTRIES=10000
(1077-1081).

## Host wire-behavior facts

The [HOST-FACT] table. Spec references in parentheses; code cites are RR1-46.BAS.txt.

- **SYN count on TX: exactly 2** (`TXPACK` 3829-3830), then STX, UA+65, MT (3831-3833). No padding, no trailing bytes after ETX. (Spec p.3, p.5.)
- **DLE escaping on TX: ALL message bodies, including I.** `TXPACK` is the single TX path for I, P and T; only P (MT=80) skips the body loop (3835). The escape loop (3838-3846) inserts DLE before 2, 3, 16 regardless of message type. **I bodies are escaped: yes.**
- **0xFF is never escaped** (not in the escape set, 3840-3842). (Spec never asks; matches all four node reviews.)
- **P frame = 6 bytes**: FF FF 02 UA+65 'P' 03 (3829-3835, 3850).
- **UA offset: +65**, addresses validated 0-127 (`INIT` 806-811, `TXPACK` 3832). (Spec p.3, p.5.)
- **Serial format: 8N2** — LCR = 7 written directly (`INIT` 983), with an explicit comment that 2 stop bits are "recommended especially at the higher baud rates of 57600 and 115200" and that 8N1 (LCR=3) is an acceptable substitute at ≤28800 (985-994). (Spec p.2 says 1 stop bit — see F7.)
- **Baud rates: 9600/19200/28800/57600/115200** via divisor latch 12/6/4/2/1 (824-828, 979-982). Exactly the spec p.4 list.
- **dH/dL actually sent: 0,0** — `DL=0` (`INITRR` 1080), split high/low at `INIT` 999-1000. (Spec p.6.)
- **NDP types supported: M, N, X only. No C** (`INIT` 834-841 — any other NDP$ aborts init). This Host predates cpNode.
- **I-before-first-poll: yes, strictly** — `INITRR` sends I to all five nodes back-to-back at program start (`RR1-46.BAS.txt:278`), before the first `readrr` (281). **I is never re-sent for the life of the program.**
- **No delay between the I messages** or between I and the first poll beyond QBASIC execution time.
- **Poll list: hard-coded, ascending UA 0,1,2,3,4**, inlined in `readrr` (1730, 1767, 1801, 1832, 1865). Every node polled once per main-loop pass; no per-node scheduling, no skipping.
- **T sent every cycle to every node, full output image** — `writerr` unconditionally repacks and sends all NO bytes each pass (no change detection). T order: UA 4 first, then 0,1,2,3 (4222-4223, 4286-4472).
- **Poll pacing: none.** The loop free-runs; cycle time is whatever serial + logic takes (displayed on screen, line 300). At 9600 8N2 the serial traffic alone for this roster (~178 bytes/cycle) floors the cycle at roughly 200 ms, i.e. ~4-5 Hz.
- **Timeout: per-BYTE, iteration-count based, not wall-clock.** `RXBYTE` polls LSR bit 0 up to MAXTRIES=10000 times (2103, 2117-2124; MAXTRIES set at 1081). The counter resets on every byte, so there is no whole-message deadline. Real-time value is CPU-speed dependent (order tens of ms on period hardware; unverifiable precisely — see F3/F21).
- **On timeout: abort this node's read, keep stale data, poll next node.** `ABORTIN=1` → `INPUTS` returns (1310 etc.); `readrr` unpacks the untouched `ib()` array (stale hold); the loop proceeds to the next node. No retry, no re-init, no dead-node marking, no backoff, no counters (print-only diagnostics).
- **On malformed reply (wrong UA, MT≠'R', unescaped 2 or 3 in body): immediate re-poll of the same node**, unbounded (`GOTO REPOL`, 1320, 1325, 1332-1333).
- **R-parse: SYNs are ignored entirely** — the receiver hunts for a bare 0x02 (GETSTX, 1308-1311); zero, two, or twenty SYNs are all accepted.
- **R-parse expects exactly NI data bytes then ETX** (1329-1341); verifies UA and MT of the reply (1316-1325).
- **No TXEN / RS-485 turnaround handling anywhere.** TX waits only for THR-empty (LSR bit 5) per byte (3855-3858) and returns with the last byte still shifting. The Host assumes the spec p.4 four-wire full-duplex RS-422/485 network behind an always-driving converter ("dongle").
- **Inter-byte TX gaps are possible and were normal.** Byte-at-a-time THR-polled TX from interpreted/compiled BASIC means inter-byte gaps of interpreter-loop magnitude, especially at 57600+. Classic Nodes were therefore built with **no inter-byte timeout expectation** — corroborates comparison.md §3's warning that only *newer* node code chokes on mid-frame pauses.
- **SMINI geometry enforced: NI=3, NO=6** (846-854); NS = yellow-oscillation bit-pair count 0-24, CT(1..6) sent only when NS>0 (856-905, 1018-1028). **SUSIC: NS=1-16 card sets** (916), CT encoded 2 bits/card (01=in, 10=out, 00=none — comment at 1054), N→×3 ports/card, X→×4 (962-963), NI/NO cross-checked against the CT array (961-974).

## Findings

### F1 [HOST-FACT] The reference Host escapes every body it transmits — I messages included
`TXPACK` (RR1-46.BAS.txt:3838-3846) is the only transmit path; `INIT` routes the
I body through it (1031) exactly as `OUTPUTS` routes T (1709). The escape loop
runs for any MT except P. This is not theoretical for I: SUSIC CT=2 ("OXXX",
spec Table 1 p.7) is a legal last-set value here (926), and SMINI CT=3 is a
legal yellow-pair mask (869) — both would be emitted escaped. **This code is
the origin of the de-facto "escape all bodies" rule** documented in
comparison.md §1.1/§2: the spec's normative reference implementation always
escaped I bodies, so every fielded classic Node was initialized with escaped I
bodies from day one.

### F2 [HOST-FACT] 8N2 on the wire, 8N1 sanctioned only below 28800
`OUT PA+3, 7` (983) = 8 data, no parity, 2 stop bits, with the author's comment
(985-994) recommending 2 stop bits at 57600/115200 and offering LCR=3 (8N1) as
an optional speed-up at ≤28800. This settles the "classic Chubb practice = 8N2"
claim in comparison.md §1.2 with a primary citation.

### F3 [HOST-FACT] The poll timeout is an iteration count (MAXTRIES=10000 LSR polls), per byte, not per message
`RXBYTE` (2103, 2117-2124) counts loop passes, each doing two `INP` reads plus
interpreter overhead; `INTRIES` resets to zero at the start of every byte
(2103). Consequences: (a) the timeout has no defined wall-clock value — it
scales inversely with CPU speed; (b) there is no message-level deadline — a
Node trickling one byte per ~timeout period can extend a single poll
indefinitely; (c) the effective "poll timeout" for a totally silent node is one
MAXTRIES period (the STX hunt's first `RXBYTE` aborts). A new Host must not
copy this: use wall-clock per-byte AND per-message deadlines.

### F4 [HOST-FACT] Recovery policy: skip-and-hold on timeout, unbounded re-poll on malformed, re-init never
On timeout the Host abandons the node's read and *continues using the previous
cycle's input image* (see F20). On a malformed reply it immediately re-polls
the same node with a fresh P (1320, 1325, 1332-1333) with no retry cap (F14).
Nothing is ever re-initialized after startup (F22). Spec p.8 only requires
"handle a timeout error, and the next Node in the poll list is polled" — this
Host conforms, and defines how little fielded Nodes could expect from a Host.

### F5 [HOST-FACT] No line-turnaround discipline; TX pacing is THR-empty per byte
`TXPACK`'s send loop (3853-3867) waits for LSR bit 5 (THR empty) before each
byte and returns without draining the shifter (no TEMT/bit-6 wait). There is no
TXEN, no RTS toggling, no deliberate inter-message gap. Works only because the
network is 4-wire full-duplex (spec p.4) with an externally powered converter.
A Host targeting 2-wire RS-485 gets zero guidance from the reference lineage —
comparison.md §2's assert→write→flush→deassert rule stands on the node
reviews alone.

### F6 [SPEC-DEVIATION] I-body escaping deviates from the spec's letter — and is the de-facto standard
Spec p.5-6 §D.a mandates DLE insertion only "when forming a Transmit Data or
Receive Data message." This Host escapes I bodies too (F1). Tagged a deviation
against the spec text, but it *is* the behavior the entire installed base was
built against; the spec text is the defect (comparison.md §1.1). CMRInet
must escape I bodies.

### F7 [SPEC-DEVIATION] 8N2 framing vs the spec's "1 Stop bit"
Spec p.2 (§A, ~line 55) defines framing as 10 bits: 1 start, 8 data, 1 stop.
The reference Host transmits 11-bit characters (F2). Harmless to 8N1 receivers
(extra stop = idle) but it means classic Nodes' replies were *received* by a
UART configured 8N2 — an 8N1-transmitting Node's back-to-back bytes could in
principle produce framing errors at the Host. Never checked: `RXBYTE` reads LSR
but only tests bits 0 and 1, ignoring framing-error bit 3 (2110-2129).
Confirms comparison.md §1.2: TX 8N2, RX tolerant, is the interop-safe choice.

### F8 [SPEC-DEVIATION] The Host receiver ignores SYN entirely (tolerates 0..n SYNs)
Spec p.3/p.5 says every message begins with two SYNs; the GETSTX hunt
(1308-1311) discards everything until a 0x02 appears, so replies with zero,
one, or many SYNs are all accepted. This tolerance is itself de-facto: Nodes
whose SYN emission was sloppy would never have been caught by the reference
Host. (The kernel node review found the mirror image: bare STX starts a frame
there too.) A bench-instrument Host should *count and report* SYNs rather than
require or ignore them.

### F9 [SPEC-AMBIGUITY] "Timeout interval" is undefined by the spec; the reference resolution is a CPU-bound try counter
Spec p.8 names "a specified time, known as the timeout interval" without units,
value, or scope (byte vs message). The reference lineage resolves it as
MAXTRIES status-register polls per byte (F3). Any wall-clock number a new Host
picks is therefore an invention; comparison.md §2's "per-node configurable,
default ~100 ms" recommendation is consistent with the loose reference
behavior.

### F10 [SPEC-AMBIGUITY] Host behavior on a malformed R is unspecified; the reference re-polls immediately
The spec defines Node addressing/discard rules (p.5) and Host timeout (p.8) but
never says what a Host does with a corrupt or misaddressed reply. This Host
prints and re-polls (F4). Fielded Nodes therefore may legitimately receive
back-to-back P messages with no intervening reply consumed — a Node that
buffers its reply and a re-poll arrives mid-transmit must cope. Worth an
explicit test case in the CMRInet testbed.

### F11 [BUG] The STX hunt is not DLE-aware — escaped 0x02 residue can lock onto a false frame
GETSTX (1308-1311) scans raw bytes for 0x02 with no DLE state. Residue from an
abandoned reply (see F12) or a desynced stream containing the legal sequence
`DLE 02` will match on the 02: the next byte (real data) is then interpreted as
UA, almost always mismatching → error print → `REPOL` sends *another* P
(1320). Failure scenario: one over-long reply leaves residue → false STX →
re-poll → the node now queues a second full reply → more residue. The loop
does eventually resync (replies are finite and the hunt discards), but the
transaction storm and multi-cycle input staleness are real. A DLE-aware hunt
(or flush-to-ETX with DLE skip, as the spec's node-side rule p.5 implies)
fixes it.

### F12 [BUG] On ETX mismatch the data is used anyway and the residue poisons the next transaction
After NI data bytes, `INPUTS` checks for ETX but only *prints* on mismatch
(1339-1341) — no re-poll, no flush, and the already-stored `ib()` is consumed
by `readrr`. If a Node's real NI exceeds the Host's configured NI (geometry
mismatch, node firmware change), every cycle reads the first NI bytes as data,
mis-frames the remainder, and leaves bytes in the FIFO for the next node's
GETSTX to chew through (see F11). Concrete consequence: a single mis-configured
node degrades *other* nodes' polls. A new Host must validate length and flush
to a clean boundary before proceeding.

### F13 [BUG] A babbling node stalls the Host forever
The per-byte try counter resets on every received byte (2103), and GETSTX
discards non-STX bytes without any cap (1308-1311). A Node (or noise source)
emitting a continuous stream of non-0x02 bytes keeps `INPUTS` in the hunt loop
indefinitely — the CTC machine's entire main loop is inside this call, so the
whole railroad freezes. No watchdog exists. The message-level deadline missing
from F3 is the fix.

### F14 [BUG] The malformed-reply retry loop is unbounded
`GOTO REPOL` (1320, 1325, 1332-1333) has no attempt counter. A Node that
consistently replies promptly-but-corruptly (e.g. persistent wrong UA due to an
address clash — two nodes answering the same poll and colliding) pins the Host
on that node forever, with a P-storm on the wire. Escape requires the node to
fall silent long enough for a MAXTRIES abort. New Host: cap retries per poll
cycle (comparison.md §2 miss-counter approach covers this).

### F15 [BUG] DLE-then-timeout stores a fabricated 0x00 as input data
In the body loop, the DLE branch `IF INBYTE = 16 THEN CALL RXBYTE` (1334) does
not re-check `ABORTIN` before `ib(i) = INBYTE` (1335). If the reply dies right
after a DLE, `RXBYTE` aborts with `INBYTE = 0` (2122) and 0x00 is committed as
the input byte for that port; the abort is only noticed on the *next* loop
iteration (1331) — and if the DLE was the last data byte, not even then: the
ETX check at 1339 runs, `ABORTIN` is still 1 from the aborted read... line 1340
catches it, but `ib(i)=0` was already stored and is used. For occupancy inputs
0 = VACANT (line 181): a truncated frame can report an occupied block as vacant
— on a CTC machine that is a signal-clearing hazard. Textbook argument for
comparison.md §3's staging-buffer/commit-on-ETX rule.

### F16 [BUG] `INTERR` typo disarms the SUSIC geometry cross-check
Lines 968 and 973 assign `INTERR = 1` (creating a fresh variable) instead of
`INITERR = 1`. The NI/NO-vs-CT consistency errors are printed but the error
flag is never set. Compounded by F19 (the flag is advisory anyway), but it
shows this validation path was never exercised to failure. QBASIC's implicit
variable creation hides it; `DEFINT A-Z` (line 42) doesn't help.

### F17 [BUG] Bad-UA diagnostic prints the wrong variable
`PRINT "ERROR; Received bad UA = "; ib` (1320) prints the scalar `ib` (distinct
from the array `ib()` in QBASIC, always 0), not the received byte. The single
most useful desync diagnostic — *which* address actually replied — is lost.
Trivial, but it directly cost debuggability on the layout this ran.

### F18 [BUG] `CT` is dimensioned to 15 but NS is validated up to 16
`DIM SHARED ... CT(15) ...` (148) allows indices 0-15; `INIT` accepts NS up to
16 for SUSIC (916-919) and loops `FOR i = 1 TO NS` reading `CT(i)` (924) and
loading `OB(LM)=CT(i)` (1012-1015). NS=16 → `CT(16)` → QBASIC "Subscript out
of range" → program halt. Latent here (this layout uses NS=4, line 1118) but a
maximally-configured SUSIC crashes the reference Host at init.

### F19 [DESIGN-LIMITATION] Validation failures don't stop transmission — INITERR is write-only
Every parameter check sets `INITERR` (804-918), but nothing ever reads it (grep
confirms: set at 10 sites, tested nowhere), and `INIT` proceeds through UART
setup and `TXMSG` regardless. Only an invalid NDP$ short-circuits (841 →
INITRET — which also skips UART programming entirely). So a Host with, e.g.,
UA=200 configured prints an error and then happily transmits UA+65=265→9
(16-bit int, then `OUT` truncates) — a wild frame on the wire. The
application never learns init failed.

### F20 [DESIGN-LIMITATION] Timeout/abort is invisible to the application — inputs silently freeze
`readrr` calls `INPUTS` and unconditionally unpacks `ib()` (1730-1731 etc.);
`ABORTIN` is never checked outside `INPUTS` itself (grep: tested only at
1310-1340). On a dead node the Host displays and acts on the last good input
image indefinitely — occupancy lamps stay green/red forever with no staleness
indication other than scrolling console prints. For CMRInet: per-node
freshness state must be a first-class API output, not a print.

### F21 [DESIGN-LIMITATION] The timeout constant must be hand-tuned to the CPU
MAXTRIES=10000 (1081) was chosen for period hardware; on a faster machine the
same constant can elapse in *less than one character time* at 9600 baud,
producing spurious aborts mid-reply on perfectly healthy nodes (each abort then
triggers F15/F12-class fallout). The classic C/MRI manuals' advice to tune
MAXTRIES per machine is the tell that this bit users repeatedly.

### F22 [DESIGN-LIMITATION] No re-initialization, ever — a power-cycled SUSIC never recovers
I messages are sent once at startup (278, `INITRR`). A SUSIC that resets
mid-session loses its card table and will not answer polls correctly until the
*operator restarts the Host program*. The Host has no notion of "node was
silent N times → re-INIT" (comparison.md §2's recommendation). SMINI nodes
with NS=0 mostly get away with it (fixed geometry); card-table nodes don't.

### F23 [DESIGN-LIMITATION] Fixed buffers cap geometry; worst-case escaping overflows TB and halts the program
`DIM OB(60), ib(60), CT(15), TB(80)` (148). TX worst case: 5 header + 2×LM
(all bytes escapable) + 1 ETX → ETX lands at index 5+2·LM+1; LM=38 already
exceeds TB(80) → runtime halt. NO/NI beyond 60 (legal for large SUSICs: X-type
supports up to 256 ports per spec-era hardware) are unrepresentable. Safe for
this layout (max LM=32), but the lineage's sizing habits must not be inherited:
size for 256-byte bodies × 2 (post-stuffing), per comparison.md §1.7.

### F24 [STRENGTH] Hardware overrun detection with per-node attribution
`RXBYTE` checks LSR bit 1 (overrun) on every poll and prints "PC overrun at
node = UA, LSR = …" (2110-2114). Primitive but genuinely useful: it catches
the Host falling behind the wire — exactly the class of self-inflicted loss a
polled single-byte-FIFO design suffers. A bench Host should keep (and count,
not print) the equivalent: RX-overrun, framing, and parity counters per node.

### F25 [STRENGTH] Exhaustive, table-driven configuration validation before anything touches the wire
The SMINI CT whitelist (867-905, valid bit-pair masks with per-value signal
counts cross-summed against NS) and the SUSIC CT whitelist (924-957, including
partial-set codes accepted *only in the last set*, 925) plus the NI/NO-vs-CT
port-count cross-check (961-974) encode more C/MRI geometry law than the spec
itself (which defers to Ref [2] pages 9-14/12-8, spec p.7). This is the best
available machine-readable statement of what dH/dL/NS/CT values a classic Node
was ever legitimately initialized with — mine it for CMRInet's config
validator and test vectors. (Weakened in execution by F16/F19, but the tables
are correct.)

### F26 [STRENGTH] The Host verifies reply UA, MT, and escape discipline — this trained the fielded ecosystem
`INPUTS` rejects replies whose UA doesn't match the polled node (1320), whose
MT isn't 'R' (1325), and treats *unescaped* 0x02/0x03 inside the body as a
protocol error (1332-1333). Consequence: any classic-era Node that failed to
DLE-escape its R bodies produced visible errors and re-polls against the
reference Host — which is *why* comparison.md §2 can state that fielded nodes
escape correctly on TX. The new Host should keep these checks (as counters +
strictness knob) precisely because they define the conformance bar.

### F27 [STRENGTH] Built-in cycle-time display and byte-level trace hooks
The main loop prints its own cycle time in ms every pass (300-301), and both
`RXBYTE` and `TXPACK` carry commented-out per-byte trace prints with an honest
warning that enabling them slows the machine enough to need a nonzero DL
(2138-2141, 3862-3866) — the reference lineage acknowledging that tracing
perturbs timing. Both features (loop-time telemetry; per-byte trace that is
OFF by default and known-perturbing) belong in the bench instrument, done
properly: timestamped ring-buffer capture instead of console prints.

## Diagnostics & emulation features worth borrowing

For the CMRInet bench instrument:
- **Per-node UART error attribution** (F24): overrun/framing/parity counters keyed to the node currently being polled — the reference Host proves even LSR-bit checks catch real events.
- **Header/escape conformance checks as counters** (F26): bad-UA, bad-MT, unescaped-2/3-in-body, ETX-mismatch — the reference Host's five PRINT sites (1320, 1325, 1332, 1333, 1341, 2113, 2121) are exactly the right event taxonomy; make them per-node statistics with last-N capture instead of scrolling text.
- **Cycle-time telemetry** (F27): poll-cycle duration is the Host's single most useful health number; display it continuously as this code does (300).
- **Perturbation-aware tracing** (F27): byte traces exist but are off by default and documented as timing-destructive — adopt the stance, fix the mechanism (timestamped capture buffer, decoupled from the poll loop).
- **Config validation tables as test vectors** (F25): the SMINI CT whitelist and SUSIC CT/geometry cross-check are ready-made truth tables for the emulator's I-message generator and for fuzzing node-side init parsers.
- **What's absent is equally instructive**: no re-init, no retry cap, no staleness indication, no message deadline — the bench Host should implement each and be able to *emulate the reference behavior* (a "classic Chubb mode": 8N2, iteration-style generous timeout, immediate re-poll, never re-init) to reproduce how fielded layouts actually behaved.

## Spec ambiguities encountered

- **Timeout interval** (spec p.8): no units/value/scope. Reference resolution: MAXTRIES=10000 LSR polls per byte, CPU-dependent, no message deadline (F3, F9).
- **Malformed-reply handling** (spec p.5, p.8): unspecified for the Host. Reference resolution: print + immediate unbounded re-poll (F10, F14).
- **DLE scope** (spec p.5-6 §D.a): T/R only per the text; reference practice escapes I too (F1, F6) — primary-source confirmation of comparison.md §1.1, from the very lineage the spec cites as its reference.
- **Stop bits** (spec p.2): "1 Stop bit" vs reference 8N2 with in-code rationale (F2, F7) — the spec text and its reference implementation disagree.
- **SYN requirement on receive** (spec p.3/p.5): reference Host requires none (F8); "two SYNs" is a TX-side convention only.
- **NS/CT semantics** (spec p.6-7 defers to Ref [2]): this code is the most precise open encoding of the deferred algorithm — SMINI CT = bit-pair masks summing to NS; SUSIC partial-set CTs legal only in the final set; N=×3/X=×4 port multipliers (865-974).

## Implications for CMRInet

Where this review confirms comparison.md §2 (now with reference-lineage authority):
- **Escape 2/3/16 in every body including I; never escape 0xFF** — the reference Host has always done this (F1). Settled.
- **Exactly two SYNs on TX** (F/table) — settled; and since the reference Host *ignores* SYNs on RX (F8), a master that requires them on RX would be stricter than the ecosystem's parent. Count them; don't require them.
- **8N2 TX as default, RX tolerant** (F2, F7) — settled with citation.
- **Full output image every T, every cycle** (table) — the reference Host never sent partial or change-only T frames; nodes have never seen anything else from this lineage.
- **dH/dL = 0** (table) — settled.
- **Timeout must be wall-clock, per-byte AND per-message, per-node configurable** — the reference's iteration counter (F3, F13, F21) is the cautionary tale, not the model.
- **Miss counter → re-INIT → operator alert** — the reference does none of this (F4, F22); comparison.md §2's recommendation is an improvement over, not a distillation of, the reference lineage. No conflict, but be aware fielded classic Nodes have *never been re-INITed mid-session by this Host family* — re-INIT paths in old node firmware are consequently under-exercised and deserve bench testing.
- **Retry caps** on malformed replies (F14) and **length validation + flush on ETX mismatch** (F12) — both absent in the reference; both mandatory in the new engine.
- **Commit-on-ETX staging** — F15's vacant-block hazard is the sharpest single argument in any of the five reviews for never exposing partial bodies to the application.

New facts this review adds to the record:
- The de-facto conventions weren't invented by node authors — they mirror this Host. Where nodes and spec disagree, assume the Chubb SPS lineage defines the truth (I-escaping, 8N2, SYN-tolerance, skip-and-hold timeout semantics).
- **Inter-byte TX gaps are historically normal** (F5/table): the reference Host could never guarantee gapless frames, so classic nodes tolerate arbitrary inter-byte spacing. Newer node code that requires gapless frames (comparison.md §3) is deviating from history — the master's conformance-test mode should include a "slow byte-spaced TX" case to flush those nodes out.
- **A "reference emulation mode"** (classic timeout generosity, immediate re-poll, no re-init, 8N2) is cheap to build from this review's HOST-FACT table and gives the bench instrument a ground-truth baseline for A/B-ing node behavior against the environment it was actually fielded in.
- The SUSIC/SMINI validation tables (F25) should be lifted wholesale into the master's node-table config checker; they are the only executable spec of CT semantics available.
