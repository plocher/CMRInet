# CMRI-Controller (SBHRS) — CMRInet Host-side review

## Scope & sources

- **Implementation reviewed**: `CMRI-Controller` — Turbo-C/Borland C++ era Host
  ("master") program for a USIC-based C/MRI installation at the South Bay
  Historical Railroad Society, written 2005–2014 (file headers date the core
  protocol modules to 1998–1999) by John Plocher (`README.md:1-16`). Source:
  `/Users/jplocher/Dropbox/workspace/CMRI-Controller` (~30 `.cpp/.h` files).
  This Host **pre-dates JMRI's C/MRI support** and was among the resources
  consulted when it was written; its wire behavior is part of the de-facto
  Host baseline that fielded nodes were tuned against.
- **Spec**: NMRA LCS-9.10.1 CMRInet Protocol v1.1 (Dec 2014),
  `docs/lcs-9.10.1_cmrinet_v1.1.pdf` (9 pp.). Cited as "spec p.N".
- **Prior research built upon (not repeated)**: `docs/research/comparison.md`
  §1 (spec defects) and §2 (de-facto wire conventions from the four node-side
  reviews).
- Protocol-relevant files reviewed in full: `usic.h`, `usic.cpp` (frame
  build/parse), `port.h`, `port.cpp` (UART), `cards.h`, `cards.cpp` (CT
  computation), `rroperat.cpp` (main loop/poll engine), `outbuf.cpp`,
  `panic.cpp`, `globals.h`, `rr.h`; diagnostics/emulation files `keyboard.cpp`,
  `rrdebug.cpp`, `rrlog.cpp`, `log.cpp`, `testchub.cpp` (+`-orig`/`-new`,
  byte-identical to each other), `test.cpp`, `testwire.cpp`, `util.cpp`,
  `crossenv.cpp`, `kbdio.cpp`, `makefile`, `makefile.unix`, `doc-localwiring`.
  Review was read-only; nothing was compiled or executed.

Line references are to the files as they exist on disk today.

## Architecture summary

Two-class protocol stack under a monolithic DOS application:

- **`Port`** (`port.h:9-31`, `port.cpp`) — one byte in/out. Two compile-time
  back ends: MSDOS (direct 8250/16550 UART register I/O at 0x3F8/0x2F8/…,
  `port.cpp:42-68`) and a Unix/termios path (SunOS/Solaris device names,
  `port.cpp:75-119`). `ReadByte()` returns 0=byte, 1=nothing (Unix only),
  −1=error; `WriteByte()` spins on THR-empty then writes (`port.cpp:235-245`).
- **`USIC : public Port`** (`usic.h:32-71`, `usic.cpp`) — frames and parses
  CMRInet messages for **one node** (`UnitNumber`, fixed 0 in production,
  `rroperat.cpp:206`). `FormPacket()` builds+sends I/P/T frames
  (`usic.cpp:119-168`); the constructor sends I once (`usic.cpp:101-113`);
  `Write()` sends a full-image T (`usic.cpp:173-178`); `Read()` sends P and
  synchronously parses the R reply (`usic.cpp:269-358`) using two helpers:
  `waitfor()` (hunt for a byte, discarding others, `usic.cpp:184-227`) and
  `expect()` (require next byte, `usic.cpp:230-266`).
- **Main loop** `RRLOOP()` (`rroperat.cpp:842-866`): poll/read → application
  logic → screen update → `Write()` T. Free-running, no timer. A parallel
  **emulation mode** (`/n`, `options.nochubb`) replaces the wire with an
  in-memory layout model driven from the keyboard (`rroperat.cpp:201-203`,
  `keyboard.cpp`), with `Port::NONE` making all serial I/O a no-op
  (`usic.cpp:93-95,174-176,279-281`, `port.cpp:36-40,139-142,227-229`).

Node types supported: classic USIC/SUSIC 24-bit ("N") only. No SMINI, no
32-bit SUSIC ("X"), no CPNODE ("C").

## Host wire-behavior facts

The settled [HOST-FACT] table. Spec cites verify the requirement; code cites
verify the observed behavior.

- **SYN count on TX: exactly 2** (0xFF, named `FRAME`), then STX. No extra
  padding bytes before, between, or after frames. `usic.cpp:128-131`,
  `usic.h:68`; spec p.3, p.5 (l.144-146).
- **DLE escaping on TX: every non-P body — I and T alike.** `FormPacket()`
  escapes 2 (STX), 3 (ETX), 16 (DLE) for any `mestype != 'P'`
  (`usic.cpp:134-147`). **0xFF is never escaped.** This Host ran for years
  against real USIC hardware, making it primary evidence that classic USIC
  firmware DLE-processes I bodies — i.e. that the de-facto "escape ALL
  bodies" rule (comparison.md §2) is what fielded classic hardware expects,
  despite the spec mandating DLE only for T/R (spec p.6 l.166-171).
- **UA offset: +65** (`OFFSET`, `usic.h:61`, `usic.cpp:131`); spec p.5
  (l.148-151). On RX the reply's UA **and** MT (`'R'`) are strictly verified
  (`usic.cpp:299-304`).
- **Stop bits: 8N1 on both platforms.** DOS: LCR = 0x03 = 8 data, no parity,
  1 stop (`port.cpp:65-68`, comment "Assume 81N"). Unix: `CS8` without
  `CSTOPB` (`port.cpp:115`). Spec itself says 8N1 (p.2 l.55), so this Host is
  conformant; it is an 8N1 data point on the Host side of the 8N1/8N2 split
  documented in comparison.md §1.2.
- **Baud: 9600 in production** (`rroperat.cpp:206`); divisor table supports
  only 150–19200, and any unrecognized rate silently becomes 19200
  (`port.cpp:49-59,84-94`). No 28800/57600/115200 despite spec p.4 (l.122).
- **dH/dL: NON-ZERO — dH=0, dL=200 → 2000 µs (2 ms) node transmit delay.**
  `USIC::DELAY = 200` (`usic.h:58-60`, comment: "keep the interface from
  overrunning the CPU. About 10uS per unit"), encoded big-endian-ish as
  `DELAY/256`, `DELAY%256` (`usic.cpp:104-105`). This is the living reason
  the dH/dL field exists: a DOS-era Host that couldn't drain the UART fast
  enough asked the node to pace its R bytes. Spec p.6 (l.203-206) says
  "set to zero for modern Host computers."
- **NDP: 'N' only** (`usic.cpp:103`) — with a wrong comment ("No CRC check");
  'N' actually means USIC/SUSIC-24-bit (spec p.6 l.198).
- **NS/CT construction: matches spec Table 1 exactly.** NS = number of 4-card
  sets; each CT byte packs I=1, O=2, none=0 into 2 bits per card, first card
  in bits 0-1 (`cards.cpp:37-45,55-90`). Spot-checks: IXXX=1, OXXX=2, IIXX=5,
  OOXX=10, IIII=85, OOOO=170 — all agree with spec p.7 Table 1. Max 16 sets
  (64 cards), bounds-guarded (`cards.h:41`, `cards.cpp:55-61`).
- **I-before-first-poll: yes; I sent exactly once, ever.** The I frame goes
  out in the USIC constructor (`usic.cpp:101-113`) before the first
  `Read()` (`rroperat.cpp:206,220`). There is **no re-init policy** of any
  kind — no retry, no re-INIT after silence, no periodic refresh.
- **Startup sequence on the wire: I → P → (R) → …app cycle… → T.** The first
  T is sent at the end of the first `RRLOOP()` pass (`rroperat.cpp:864`), not
  immediately after I.
- **Poll engine: single node, free-running synchronous cycle.** Every loop:
  P → block until full R parsed → process → T (`rroperat.cpp:842-866`).
  **T is sent every cycle, full output image, regardless of change**
  (`usic.cpp:173-178`, length = `NumberOutputPorts()` = 138 bytes at SBHRS).
  No poll list, no per-node scheduling, no pacing timer — the 2 ms/char
  node-side delay and the DOS screen update are the only pacing.
- **Timeout: not time-based, and platform-divergent.**
  - DOS: `ReadByte()` **blocks indefinitely** spinning on UART LSR; after
    200,000 status polls it begins checking the keyboard for 'Q' (exit) and,
    with `/q` (`autoquit`), calls `exit(-99)` (`port.cpp:153-185`,
    `rroperat.cpp:57`). The `waitfor()`/`expect()` "nothing to read" paths
    are dead code on DOS.
  - Unix: termios `VMIN=0, VTIME=5` → each `read()` waits ≤0.5 s
    (`port.cpp:116-117`); `waitfor()` gives up after `MAX_RETRIES`=10 empty
    reads (~5 s worst case, `usic.cpp:181,207-212`) → `Read()` does
    `goto RESTART` and **re-polls forever** (`usic.cpp:283-304`). `expect()`
    never gives up on empty reads (`usic.cpp:247-251`).
  - Net: on timeout the Host **retries the same node indefinitely**; there is
    no "next node in poll list" (there is no poll list), no miss counter, no
    dead-node marking. Spec p.8 (l.243-247) envisions a timeout interval and
    moving on; this Host predates that formalization.
- **RX framing tolerance: SYNs not required.** `Read()` hunts for STX,
  discarding anything else including SYNs (`usic.cpp:295-298`); accepts an R
  frame with 0, 2, or N SYNs. Raw 0xFF inside the body is treated as data
  (`usic.cpp:309-341` has no 0xFF case).
- **RX length policy: fixed expectation = configured NI** (54 bytes at
  SBHRS). Short reply → the ETX is hit inside the data loop as an unescaped
  ETX → abort+re-poll; long reply → `expect(ETX)` fails → abort+re-poll
  (`usic.cpp:316-326,345-347`). Length is thus validated implicitly.
- **Line discipline: full-duplex RS-232 assumed.** No TXEN/RTS turnaround
  handling anywhere; no deliberate inter-message gaps or post-T delays on the
  Host side. (Layout wiring per `doc-localwiring` and the spec's "dongle"
  model, spec p.4 l.130-133.)

## Findings

### 1. [BUG] Every I message carries one extra uninitialized byte
`usic.cpp:102-113` builds the I body with `messagelength` starting at 1 and
post-incrementing per byte: after `'N'`, dH, dL, NS and NS CT bytes the body
occupies `initbuf[1..4+NS]` but `messagelength` ends at `5+NS`.
`FormPacket()`'s body loop `for (i = 1; i <= meslength; i++)`
(`usic.cpp:136-146`) therefore transmits `initbuf[5+NS]` — an uninitialized
stack byte — as a trailing body byte. The T path is correct
(`usic.cpp:177`: length = 138 = exactly the 1-based payload), so the
convention mismatch is specific to the constructor. Consequence: the wire I
frame is `<'N'><dH><dL><NS><CT(1..NS)><garbage>` (garbage DLE-escaped if it
happens to be 2/3/16). Spec p.6 (l.190) defines no such byte. That this Host
worked in the field implies (inference, not verified against firmware) that
classic USIC firmware consumes exactly NS CT bytes and flushes the remainder
to ETX — a tolerance a new Host should *not* rely on, and a behavior the
CMRInet receiver/emulator should tolerate when mimicking node behavior.

### 2. [SPEC-DEVIATION] DLE-escapes I bodies (and this is the de-facto standard)
`usic.cpp:134-147` escapes 2/3/16 in every non-P body, including I; spec p.6
(l.166-168) mandates DLE processing only for Transmit/Receive Data. This
deviation is load-bearing: with NS=16, the raw NS byte 0x10 would otherwise be
a bare DLE inside the I body, and CT=2 (OXXX) collides with STX. Confirms
comparison.md §1.1/§2 from the Host side with fielded-hardware evidence.

### 3. [HOST-FACT] Ships non-zero transmission delay (dH/dL = 0/200 = 2 ms)
See table. Design lesson for CMRInet: dH/dL is a *Host self-defense*
knob for slow receivers, not vestigial; a bench-instrument Host should be able
to set it per-node to test node compliance (fielded nodes' dH/dL handling is
version-dependent per comparison.md §1.4). Cites: `usic.h:58-60`,
`usic.cpp:104-105`; spec p.6 l.203-206.

### 4. [BUG] TX buffer can overflow on heavily-escaped T frames
`transbuf[255]` (`usic.cpp:121`) must hold 5 header bytes + body + ETX. With
the SBHRS 138-byte output image, worst case is 5 + 2×138 + 1 = 282 bytes.
Overflow needs ≥111 escapable output bytes in one frame — improbable with this
layout's data but structurally unguarded; the escape loop performs no bounds
check (`usic.cpp:136-146`). A stack smash here corrupts the frame mid-send at
best. Anti-checklist item for CMRInet: size TX staging for 2×body+8, or
escape while streaming.

### 5. [BUG] DOS receive path has no timeout at all — a silent node hangs the Host
`port.cpp:153-185`: `ReadByte()` spins on LSR until a byte arrives. After
200,000 iterations (CPU-speed-dependent, not time-based) it only offers a
keyboard 'Q' escape, or `exit(-99)` with `/q` (`port.cpp:155-163`,
`rroperat.cpp:57`). So on the production platform, one unplugged cable froze
the entire dispatcher UI (hence the `/q` "auto-quit when chubb I/O hangs"
flag — evidence hangs were a real operational problem). This is the same
blocking-read defect the node reviews flagged (comparison.md §3, item 1),
here on the Host side.

### 6. [BUG] Unix `expect()` never times out
`usic.cpp:247-251`: on "nothing to read" `expect()` does `continue` —
forever. A node that dies after the header (or a truncated R) leaves the Host
looping in 0.5 s `read()` calls indefinitely. Only `waitfor()` (the STX hunt)
has the 10-retry escape; mid-frame silence is unrecoverable without operator
action. → Inter-byte timeout must cover *every* receive state, not just hunt.

### 7. [BUG] Unix serial path enables IXON — binary R data can freeze Host TX
`port.cpp:112`: `ti.c_iflag = IXON`. An R body legitimately containing raw
0x13 (DC3/XOFF — only 2/3/16 are escaped, so 0x13 arrives bare) suspends the
Host's output until an 0x11 (XON) happens to arrive. Failure scenario: one
input byte pattern silences all subsequent P/T frames; the system appears
"hung" with no error. Classic must-disable-all-line-discipline lesson for any
POSIX-hosted CMRInet tool (termios must be fully raw: no IXON/IXOFF, no
ICRNL, etc.).

### 8. [BUG] `waitfor()` is not DLE-aware during resync
`usic.cpp:196-225` scans byte-at-a-time for STX with no escape tracking. If
an aborted frame's residue contains an escaped 0x02 (`DLE 0x02`), the DLE is
discarded as "unexpected" and the 0x02 is taken as STX → false header →
`expect(UA)` fails → another RESTART cycle. Recoverable (it re-polls), but it
converts one desync into several wasted poll cycles. CMRInet's receiver
should carry escape state through its hunt, or (better) resync only on the
SYN SYN STX sequence.

### 9. [DESIGN-LIMITATION] No recovery ladder: no retry budget, no re-INIT, no dead-node handling
On any parse failure or timeout the sole strategy is "re-poll the same node
forever" (`usic.cpp:283-347` RESTART). A node that power-cycled (lost its
config) is never re-initialized — the Host keeps polling and writing T to a
node that may no longer understand its geometry. Combined with finding 5,
node loss = Host hang. Confirms comparison.md §2's "treat missed responses as
retryable, re-INIT after repeated misses" as the gap to fill.

### 10. [DESIGN-LIMITATION] Single-node engine
`UnitNumber` is a constructor parameter but production hardcodes node 0 with
one `USIC` instance (`rroperat.cpp:203-206`); nothing iterates addresses.
Poll-list mechanics, per-node state, and per-node timeouts simply don't exist
here. (Also `serialport == Port::NONE` checks appear in USIC methods, making
the emulated-node case a per-instance property — a good seed for per-node
"real vs emulated" polymorphism in a modern Host.)

### 11. [STRENGTH] R-body parsing detects desync via unescaped STX/ETX and implicit length check
`usic.cpp:316-326`: any raw STX/ETX inside the fixed-length data window
aborts the frame ("truncated packet") rather than storing it; `expect(ETX)`
(`usic.cpp:345`) rejects over-length replies. Together with strict UA/MT
verification this means a malformed reply cannot silently corrupt `InBuf` —
it can only cost time. Several *node* implementations reviewed earlier fail
exactly this (comparison.md §3 "no length validation"); this 1990s Host got
it right.

### 12. [STRENGTH] DLE unescape ordering is correct and 0xFF-in-body is handled
DLE is checked only after the raw-STX/ETX check, and the escaped byte is
stored unconditionally (`usic.cpp:327-336`); raw 0xFF is plain data. Matches
the de-facto rules in comparison.md §2. (Nit: the byte following DLE is not
itself validated, so `DLE FF` would be accepted — harmless.)

### 13. [STRENGTH] Whole-application emulation mode behind a one-flag wire stub
`/n` (`nochubb`) constructs the USIC on `Port::NONE`; every protocol entry
point no-ops cleanly (`usic.cpp:93-95,174-176,279-281`, `port.cpp:36-40`),
and `keyboard.cpp` provides an interactive fault/stimulus injector that edits
`InBuf`/`OutBuf` directly: panels (assign/drop with mainline/cab/block
encoding, `keyboard.cpp:71-174,461-548`), turnouts
(normal/diverge/toggle/unlock/lock, incl. feedback-bit simulation,
`keyboard.cpp:177-244,559-632`), detectors (occupy/clear/toggle,
`keyboard.cpp:247-313`), cab direction/buttons (`keyboard.cpp:315-458`).
Emulation is toggleable at runtime from the Options menu
(`rroperat.cpp:765-768`). This is the strongest borrowable idea in the
codebase — see Diagnostics section.

### 14. [STRENGTH] Layered compile-time protocol tracing with human-readable frame rendering
Three levels — `DEBUG_USIC` (one line per frame:
`[FRAME|FRAME|STX|n|"T"|len{bin:bin:…}|ETX]`, `usic.cpp:150-163,348-357`),
`DEBUG_USIC_DETAILS` (per-byte RX trace: `.` per idle read, `[MNEMONIC]` per
discarded byte, OK on match — `usic.cpp:191-224,236-263`), and
`DEBUG_USIC_ERRORS` (parse errors only, `usic.cpp:312-333`). Byte naming via
a full ASCII-mnemonic table plus `FRAME` (`usic.cpp:15-48`); payloads printed
as grouped binary via `binary()` (`util.cpp:148-204`). The *taxonomy* (frame
summary / byte trace / errors-only) is worth carrying into CMRInet;
the compile-time-only aspect is not.

### 15. [BUG] Off-by-one OOB read seeding SaveBuf at startup
`rroperat.cpp:222-224`: `for (i = 0; i <= IOcard0->InputBufferSize(); i++)
SaveBuf[i] = InBuf[i];` — `InputBufferSize()` = NI+1 = 55 is itself one past
the last valid index (0..54), so `InBuf[55]` is read out of bounds (write is
in range only because SaveBuf is output-sized, 139). Harmless in practice,
but exactly the `<=` boundary pattern the node reviews catalogued
(comparison.md §3, off-by-one guards).

### 16. [BUG] `LogQ::log` sprintf misuse can overflow its 100-byte message
`log.cpp:10-11`: `sprintf(m->text, "%s: %s", tstamp+4, message, 75);` — the
`75` is a stray argument (an snprintf bound that isn't one), and
`m->text[75]='\0'` truncates only *after* a potential overflow of
`text[100]` (`globals.h:120-128`). All internal call sites pass short
literals, so latent, but any future long message corrupts the heap list.

### 17. [DESIGN-LIMITATION] Serial parameter surface is DOS-frozen
Direct UART divisor programming caps rates at 19200 and silently maps unknown
rates to 19200 (`port.cpp:49-59`); COM port = fixed I/O addresses
(`port.cpp:42-47`); the Unix port hardcodes SunOS-era device names
(`port.cpp:77-82`). What translates to modern hardware: the *policy* (8N1,
9600 default, one-byte non-blocking-ish reads, THRE-checked writes, overrun
detection — see finding 18). What doesn't: everything about how the port is
opened and configured.

### 18. [STRENGTH] UART overrun is detected and surfaced as a frame error
`port.cpp:179-183`: LSR bit 1 (overrun) aborts the read with an error, which
`Read()` turns into a frame abort + re-poll. The 2 ms dH/dL (finding 3) plus
this check are two halves of one story: the Host both *asked* the node to slow
down and *noticed* when it still couldn't keep up. A CMRInet bench
instrument should likewise count and expose UART-level overruns distinctly
from protocol-level errors.

### 19. [SPEC-AMBIGUITY] "Timeout interval" left to the implementation — here it isn't time at all
Spec p.8 (l.243-247) requires waiting "for a specified time." This Host's
units are UART status-poll iterations (DOS) or empty-read counts × VTIME
(Unix) — both CPU/OS-dependent, neither configurable. Corroborates
comparison.md §1.3 (error/recovery semantics are implementation-invented) and
argues for CMRInet's per-node millisecond timeouts.

### 20. [SPEC-AMBIGUITY] May a Host rely on node-side flush-to-ETX for unparseable body bytes?
Finding 1's extra I-body byte was evidently harmless to real USIC firmware,
and the spec's "discard until ETX" rule (p.5 l.158-160) is written for
*non-addressed* nodes only. Whether an *addressed* node must tolerate
trailing bytes between its parsed body and ETX is unspecified. CMRInet
(as node-emulator) should tolerate-and-count them; as Host it should never
emit them.

### 21. [HOST-FACT] Poll cycle semantics: P and T are unconditional, per-cycle, full-image
Settled by `rroperat.cpp:842-866` + `usic.cpp:173-178`: this Host never sends
"T only on change" and never sends partial T frames. During panic mode the
cycle continues at ~1 Hz (`panic.cpp:56-68,89-92`: blink → `Write` → `Read`
with `sleep(1)`), so nodes were fielded against continuous full-image
refresh. Agrees with comparison.md §2 ("send the full output image in every
T frame").

## Diagnostics & emulation features worth borrowing (for the CMRInet bench instrument)

This codebase's biggest contribution is not its protocol engine but its
operator-facing instrumentation. Concrete catalogue:

1. **Wire-stub emulation as a first-class mode** (finding 13). One flag swaps
   the serial layer for no-ops and routes stimulus from a keyboard-driven
   fault injector directly into the I/O images. For CMRInet: model each
   node as an object that is either "on the wire" or "simulated," with the
   same API — the SBHRS design proves the whole application stack (and its
   tests) can run hardware-free. `rroperat.cpp:201-228`, `keyboard.cpp`.
2. **Stimulus injectors organized by domain concept, not by byte**: operators
   emulate "Panel 3 assigns cab 5 to block 42," not "set bit 4 of InBuf[7]"
   (`keyboard.cpp:71-174`). A bench Host should offer both levels: raw
   byte/bit poking *and* domain-level actions with the encoding done for you.
3. **Live per-card wiring view ("cardio" screen)**: renders every card's 24
   pins as live 0/1s, mapped through *physical pin* ordering (old-style vs
   new-style Chubb card pin maps, `util.cpp:40-114`), with `+`/`-` card
   stepping and a card picker (`testchub.cpp:165-237`,
   `rroperat.cpp:265-274,721-733`). This is the single most bench-useful
   screen: it answers "what is the wire doing at pin 17 of card 22" without
   mental bit arithmetic. Borrow the pin-map indirection explicitly — modern
   cpNode/IOX geometries need the same physical↔logical translation.
4. **State-matrix debug screen**: one screen showing detectors, cab
   assignments (commanded vs echoed vs last), signal aspects (color-rendered
   from the actual output bits), turnout lock/position/motor triples, panel
   inputs (`rrdebug.cpp:56-292`). Note it derives everything from
   `InBuf`/`OutBuf` — the protocol images are the single source of truth,
   which keeps the display honest.
5. **Runtime-switchable screens + toggleable debug categories** from an
   options menu (track diagram / Chubb state / wiring / log;
   assignment/turnout/detector debug logging; `rroperat.cpp:736-839`) — no
   recompile, no restart. Contrast with the protocol tracing (finding 14),
   which *is* compile-time: bench instrument should make frame tracing
   runtime-switchable too, and keep it off the protocol port (a lesson the
   node reviews paid for, comparison.md §3 "debug channels that corrupt the
   protocol").
6. **Frame-trace rendering conventions** worth copying verbatim: named
   protocol bytes (`STX`,`ETX`,`DLE`,`FRAME`), payload bytes as `_`-grouped
   binary, one frame per line with direction implicit in MT
   (`usic.cpp:150-163,348-357`, `util.cpp:148-204`).
7. **Event log with timestamped file + in-memory scrollback with
   new-since-last-view highlighting** (`log.cpp`, `rrlog.cpp:56-99` — new
   entries render red until viewed). Cheap and operator-loved; borrow the
   "unseen entries highlighted" trick.
8. **Loop counter on screen** (`options.showloopcounter`,
   `testchub.cpp:76-84`) — a trivially cheap liveness/poll-rate indicator.
   Bench version: display polls/sec and per-node response-time stats instead.
9. **`/q` autoquit watchdog** (`rroperat.cpp:57,119`, `port.cpp:156-157`) —
   crude, but the concept (unattended operation must fail loudly rather than
   hang silently) belongs in the bench instrument as a health timeout with a
   clear error, not `exit(-99)`.
10. **Graded test programs**: `test.cpp` (logic harness with a stubbed
    `Input()` — hardcoded InBuf patterns, no hardware), `testchub.cpp`
    (interactive exerciser with cardio screen, defaults to emulation,
    `testchub.cpp:443`), `testwire.cpp` (walks card/panel data structures
    against the wiring database), plus human-readable wiring databases
    (`doc-cardwiring`, `doc-localwiring`) kept next to the code. The
    Input→Process→Output loop shape (`testchub.cpp:539-543`) recurs in all of
    them — a good skeleton for CMRInet example sketches.

What's *absent* and must be added in CMRInet's instrument: error/miss
counters, per-node statistics, RX byte-level capture (this Host can trace
only at compile time), timestamping of frames, and any notion of expected-vs
-actual latency.

## Spec ambiguities encountered

- **Timeout semantics** (finding 19) — spec names the concept, defines
  nothing; this Host demonstrates the failure mode of leaving it undefined
  (loop-count "timeouts," platform-divergent hangs).
- **I-body DLE processing** (finding 2) — this Host adds Host-side fielded
  evidence to the node reviews' unanimous #1: classic hardware was driven
  with escaped I bodies for years. The de-facto rule stands.
- **Addressed-node tolerance of unexpected body bytes before ETX**
  (finding 20) — new sub-question raised by the off-by-one I frame this Host
  actually transmits.
- **SYN preamble on receive** — spec p.3 says messages start with two SYNs,
  but says nothing about whether a receiver may/must *require* them. This
  Host requires zero (finding: RX table entry; `usic.cpp:295`), which is the
  most tolerant possible reading and matches comparison.md §1.6's "nobody
  requires more than two; some require none."
- **dH/dL applicability** — spec (p.6 l.204-206) describes node behavior only
  for SMINI/SUSIC/USIC and assumes zero from modern hosts; this Host shows
  the field in real non-zero use, but the spec still never says *which*
  transmissions are delayed (per-character vs per-message). The 10 µs/unit
  granularity is corroborated by the code comment (`usic.h:60`).

## Implications for CMRInet

Agreements with comparison.md §2 (this Host strengthens them):

- **Escape 2/3/16 in every TX body including I; never escape 0xFF** —
  confirmed from the Host side against real classic hardware (finding 2).
- **Exactly two SYNs then STX** on TX (fact table); be SYN-agnostic on RX
  like this Host (hunt for header), but carry DLE state while hunting
  (finding 8) — this Host shows the cost of not doing so.
- **Full output image every T, sent unconditionally** — this is what classic
  nodes were fielded against (finding 21). Change-only T is an optimization
  the ecosystem has no evidence for; keep it off by default.
- **Per-node, millisecond, configurable poll timeout with a retry budget and
  re-INIT ladder** — every element is a direct negation of findings 5/6/9.
  The research's proposed default (~100 ms, tunable) is consistent with what
  this Host effectively needed (2 ms/char × 54-byte reply ≈ 110 ms at SBHRS
  once the dH/dL delay is included — note how the *node-side* delay dominates
  reply latency; budget for it when a node's dH/dL is non-zero).
- **8N1 vs 8N2**: this Host transmits 8N1 (fact table), the spec says 8N1,
  and MRCS-kernel nodes are 8N1 — but comparison.md §1.2 records the
  JMRI/Chubb 8N2 practice. Recommendation unchanged: TX 8N2-capable but
  default to configurable, RX tolerant of both; this review just adds one
  more 8N1 vote on the classic-USIC side.

New guidance from this review:

- **Expose dH/dL as a real per-node setting** (default 0) rather than
  hardcoding zero: it is both a compatibility knob for slow hosts/nodes and a
  conformance-test stimulus (finding 3).
- **Bound the TX staging buffer at 2×body+8 or escape-as-you-stream**
  (finding 4); unit-test the "all bytes escapable" worst case.
- **POSIX host tooling must set fully-raw termios** — the IXON freeze
  (finding 7) is invisible until one specific data byte appears, the worst
  kind of field bug. Applies to any desktop-side test harness shipped with
  CMRInet, not the Arduino library itself.
- **Adopt the RX hygiene this Host already had** (findings 11/12): strict
  UA/MT verification, raw-STX/ETX-in-body = frame abort, expected-length
  enforcement, DLE-after-control-check ordering — plus the inter-byte timeout
  it lacked.
- **Bench instrument scope**: the Diagnostics catalogue above is effectively
  a requirements list with 20 years of operational vetting: emulated nodes
  behind the same interface as real ones, domain-level and bit-level stimulus
  injection, live pin-level card views with physical pin mapping, runtime-
  switchable frame tracing with named bytes and binary payload rendering,
  highlighted event log, liveness/rate display, and a loud (not hanging,
  not `exit()`) failure mode for dead nodes.
- **Node-emulation detail**: when CMRInet's testbed emulates a classic
  USIC node, it should accept a trailing junk byte in I bodies (finding 1/20)
  and reply only after honoring dH/dL, to faithfully reproduce the
  environment this Host created.
