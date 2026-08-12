# ArduinoCMRI (Michael Adams) — adversarial CMRInet spec review

## Scope & sources
- Spec: NMRA LCS-9.10.1 "Communications Specification for CMRInet" v1.1, December 2014 (9 pages), read in full.
- Implementation: `/Users/jplocher/Dropbox/Arduino/libraries/CMRI` at **v1.7.0** (tag `v1.7.0`, commit f4bb29e) — `src/CMRI.h` (99 lines), `src/CMRI.cpp` (266 lines), `README.md`, 10 example sketches, native unit tests (`test/test_cmri/test_main.cpp`, 211 lines).
- History note: this review was first performed against v1.5 (flat `CMRI.h`/`CMRI.cpp`), then re-verified after the repo was updated to upstream v1.7.0. Each finding below carries a **Status** line; see the delta section. Intermediate tags examined: v1.5.1 (8e8bb7a), v1.6.0 (71a8db8).
- Role: node-side (slave) only. Third-party client library targeting JMRI as host.
- Review was read-only; no code was modified, compiled, or run (git used read-only for history/diffs).

## v1.5 → v1.7.0 delta
- **v1.5.1** (c264afb "Fix out of bounds error, fixes #10"): fixed the `>` vs `>=` off-by-ones in `get_byte`/`set_byte`/`set_bit`. The `set_bit` fix over-corrected (`(pos+7)/8 >= _tx_length` wrongly rejected bits 17-23 of the last byte); fully corrected in **v1.6.0** (8719b04) to `pos / 8 >= _tx_length`. Also by v1.5.1 (PR #15, 75ff096/f948df7): `delay(50)` in `transmit()` replaced with `delayMicroseconds(50)` — the 50 ms poll-reply stall is gone.
- **v1.6.0** (8719b04): restructure (sources to `src/`), clang-format, CI workflow, PlatformIO native test scaffolding. No protocol behavior change beyond the `set_bit` correction.
- **v1.7.0**: (a) `transmit()` now DLE-escapes STX (0x02) as well as ETX/DLE (af9e1e6, with a regression test); (b) INIT ('I') messages addressed to this node are now decoded through the data path (DLE-processed, flushed to ETX) and delivered to a sketch callback via new `set_init_handler()` (31fc928); `process()`/`process_char()` now return `true` for completed INIT frames.
- **Net effect on this review**: 2 BUGs and 1 SPEC-DEVIATION fixed upstream; 1 significant NEW BUG introduced (INIT payload clobbers the output buffer); the poll-reply timing implication reverses (50 ms → 50 µs, creating a new 2-wire collision consideration); the remaining findings persist with new line numbers.

## Findings

### [BUG] A third SYN byte desyncs the preamble and drops the frame
- Status: **still present in v1.7.0** (unchanged logic).
- Spec: LCS-9.10.1 p.3 ("A message starts with two SYN ... characters provide data receivers to synchronize"), p.5 §D.
- Code: src/CMRI.cpp:175-187 (`PREAMBLE_2`, `PREAMBLE_3`).
- In `PREAMBLE_3`, any byte other than STX — including another 0xFF — resets to `PREAMBLE_1`. So the sequence `FF FF FF 02 ...` is rejected: the third 0xFF falls to the `else` branch, and the following STX is then discarded in `PREAMBLE_1`. The spec specifies exactly two SYNs, so a strictly conforming host (JMRI) works, but SYN is explicitly a *synchronization/idle* character; hosts or converters that pad with extra 0xFF (or line idle reading as 0xFF after a break) make every frame invisible to this node. The v1.7.0 test suite's resync test (test/test_cmri/test_main.cpp:180-195) covers a *lone* 0xFF in garbage but not the triple-SYN case, so this remains untested upstream.
- Failure scenario: a master that transmits 3+ SYNs for receiver settling (common on RS-485 with slow-enabling drivers) gets zero responses from this node, ever.
- Fix: in `PREAMBLE_3`, `if (c == 0xFF) stay in PREAMBLE_3;` before the STX/else tests.

### [BUG] (NEW in v1.7.0) INIT payload is written into the live output buffer, corrupting node outputs
- Status: **new in v1.7.0** (introduced by 31fc928, the fix for the v1.5 INIT bug below).
- Spec: LCS-9.10.1 p.6 §D.1 (I body carries configuration parameters, not output data); p.4 (only T messages "set the Node output port bits").
- Code: src/CMRI.cpp:198-203 (`DECODE_CMD` routes both `SET` and `INIT` to `DECODE_DATA`), src/CMRI.cpp:214-224 (`DECODE_DATA` stores bytes into `_rx_buffer` regardless of `_rx_packet_type`), src/CMRI.cpp:111-117 (`get_byte` reads the same `_rx_buffer`).
- The INIT body (`NDP dH dL NS CT()...`) is decoded into the *same* `_rx_buffer` that holds T-message output data and that `get_bit()`/`get_byte()` read. A JMRI SMINI init (`'M' 0x00 0x00 0x00`) leaves `_rx_buffer[0] = 0x4D`, so `get_bit(0..7)` reads 1,0,1,1,0,0,1,0 — the node drives those onto layout hardware until the first T message arrives. Worse, `process()`/`process_char()` now return `true` for INIT (src/CMRI.cpp:90-94), so v1.5-era sketches that treat `true` as "outputs updated, apply them now" actively latch the garbage. The `_rx_data_len` bookkeeping (src/CMRI.cpp:253) is correct for the callback, but nothing restores or protects the output image.
- Failure scenario: every JMRI connection/restart momentarily sets pseudo-random turnouts/signals/relays on any node with a registered output at low bit positions; visible as startup chatter, potentially harmful for twin-coil switch machines.
- Fix: decode INIT into a separate (or stack) buffer, or save/restore the output image around INIT decoding.

### [BUG] No inter-byte timeout: a truncated frame corrupts the next frame and applies garbage to outputs
- Status: **still present in v1.7.0** (unchanged; INIT now also flows through this path).
- Spec: LCS-9.10.1 p.2 (valid-message definition), p.8 §D.2 (the *host* has a timeout concept; nodes get no guidance — see ambiguities).
- Code: src/CMRI.cpp:214-233 (`DECODE_DATA`, `DECODE_ESC_DATA`); no timer anywhere in the library.
- Two concrete cases, both verified against the state machine:
  1. A T frame that dies before its ETX leaves the parser in `DECODE_DATA`. The next real frame's `FF FF 02 UA MT` bytes are stored into `_rx_buffer` as if they were output data (until the buffer fills), and the next frame's ETX terminates the ghost frame and returns `SET` — the sketch then drives layout outputs with `0xFF 0xFF 0x02...` garbage.
  2. A frame that dies immediately after a DLE leaves the parser in `DECODE_ESC_DATA`, which unconditionally consumes the next byte (src/CMRI.cpp:226-233) — i.e., the first SYN of the next frame is eaten as "escaped data," cascading into case 1.
- Failure scenario: one glitched/brownout-truncated transmission produces visibly wrong turnout/signal states on the next host update, not just a lost frame.
- Fix: reset `_mode` to `PREAMBLE_1` (and discard the partial buffer) if more than a few character times elapse between bytes; or double-buffer and commit only on ETX.

### [BUG] Addressed INIT ("I") message is neither parsed nor flushed to ETX — body can spoof a frame header
- Status: **fixed upstream in v1.7.0** (31fc928): `DECODE_CMD` now routes `INIT` into `DECODE_DATA` (src/CMRI.cpp:202-203), so the I body is DLE-processed and flushed to its ETX, and the payload is handed to the sketch via `set_init_handler()` (src/CMRI.cpp:57-60, 90-94). The header-spoof scenario via discarded I-body bytes is gone for well-formed escaped bodies. Residuals: the fix introduced the output-buffer clobber (new BUG above); the addressed I body is now DLE-unescaped, which the spec does not mandate for I messages (see the DLE-scope ambiguity — an unescaping node paired with a literal non-escaping host mis-terminates on a raw 0x03-valued body byte); an I body longer than `_rx_length` (default SMINI: 6 bytes, i.e. NS > 2) is silently truncated before reaching the callback (src/CMRI.cpp:219-223). Unknown message types still take the one-byte `POSTAMBLE_OTHER` bail-out (src/CMRI.cpp:206-207, 246-248) instead of flush-to-ETX, but no defined MT hits that path now.
- Original v1.5 finding (for the record): `DECODE_CMD`'s `else` consumed exactly one body byte (the NDP) then re-entered preamble hunt; body bytes containing `FF FF 02` (legal in CT bitmasks/dH/dL) could spoof a frame header and swallow the following real frame.

### [BUG] Off-by-one bounds checks in `get_byte`, `set_byte`, `set_bit` — one-byte heap read/write past buffer
- Status: **fixed upstream** — v1.5.1 (c264afb) changed `>` to `>=` in all three; v1.5.1's `set_bit` variant (`(pos+7)/8 >= _tx_length`) over-corrected and rejected valid bits 17-23 of the last byte, fully fixed in v1.6.0 (8719b04) to `pos / 8 >= _tx_length`. Current code: src/CMRI.cpp:113, 121, 136. Regression tests exist (test/test_cmri/test_main.cpp:73-102).
- Residual: **negative indices are still unchecked** — all accessors take `int`, and e.g. `get_byte(-1)` passes `pos >= _rx_length` (false) and reads `_rx_buffer[-1]`; `set_byte(-1, …)` writes before the buffer. Minor (requires a caller bug), but the guards only protect one direction.
- Original v1.5 finding (for the record): `pos == _rx_length`/`pos == _tx_length` read/wrote one past the heap block; `set_bit(24)` on a default SMINI wrote `_tx_buffer[3]` of a 3-byte buffer.

### [SPEC-DEVIATION] Poll is answered without verifying the poll's ETX — now with real collision exposure
- Status: **still present in v1.7.0**, consequence *elevated* by the v1.5.1 delay change.
- Spec: LCS-9.10.1 p.7 §D.2 (poll format includes ETX); p.2 ("Any sequence of bits not meeting the full specification of this general message format is not ... a valid message"); p.4 §C (half-duplex network).
- Code: src/CMRI.cpp:204-205 (`goto POSTAMBLE_POLL` on the `P` byte itself), src/CMRI.cpp:258-260, src/CMRI.cpp:147 (`delayMicroseconds(50)`).
- The node commits to transmitting its R reply the instant it sees `P`, before the poll's ETX has been received or checked. A corrupted frame (`FF FF 02 UA P <noise>`) still gets a full response. In v1.5 the 50 ms `delay()` masked the timing race; in v1.7.0 the reply starts ~50 µs after the `P` byte — while the host's ETX (~1.04 ms at 9600 bps) is still on the wire. On the spec's four-wire network the pairs are separate so no electrical collision occurs, but on the common 2-wire RS-485 setups this library is explicitly marketed for (Auto485 examples, examples/rs485_rx_and_tx/rs485_rx_and_tx.ino) the node's driver can be enabled while the host is still transmitting — a genuine bus-contention window of about one character time.
- Fix: add a `POSTAMBLE_POLL_WAIT_ETX` state; reply only after ETX is received.

### [SPEC-DEVIATION] R-message transmit does not escape STX (0x02)
- Status: **fixed upstream in v1.7.0** (af9e1e6): src/CMRI.cpp:155-158 now escapes STX, ETX, and DLE, satisfying LCS-9.10.1 p.5-6 §D.a in full, with regression tests (test/test_cmri/test_main.cpp:53-69, 160-177).
- Original v1.5 finding (for the record): only 0x03 and 0x10 were escaped; a data byte of 0x02 went out raw — a MUST-level violation (README.md:120 still documents only the 0x03 escape).
- Master-side note: nodes running ≤v1.6.0 remain in the field, so a master's R parser must still tolerate an unescaped 0x02 in reply bodies.

### [SPEC-DEVIATION] Initialization parameters (NDP, dH/dL transmission delay, NS, CT) are not honored by the library
- Status: **partially addressed in v1.7.0; changed behavior since v1.5**.
- Spec: LCS-9.10.1 p.6 §D.1 ("Each Node on the CMRInet network must have an initialization message sent"; `<dH><dL>` transmission delay, 10 µs units, p.6 lines 203-206).
- Code: src/CMRI.cpp:147 (fixed `delayMicroseconds(50)` regardless of commanded delay), src/CMRI.cpp:90-94 (payload now delivered to sketch callback), examples/init_handler/init_handler.ino:44-50 (shows the *sketch* computing `(dH*256+dL)*10 µs` — the library itself never applies it).
- The library still ignores NDP type (N/X/M/C — no validation) and does not implement the commanded transmission delay; it substitutes a fixed 50 µs. v1.7.0 delegates compliance to the sketch author via `set_init_handler()`, which is a reasonable design for a small library but means out-of-the-box behavior still deviates: a host commanding a non-zero dH/dL gets no delay change. The v1.5-era consequence (50 ms floor forcing masters to use ≥60 ms poll timeouts) is *gone* — replies are now fast.

### [DESIGN-LIMITATION] Blocking calls inside `process()`
- Status: **largely resolved by v1.5.1; residual**.
- Code: src/CMRI.cpp:147 (`delayMicroseconds(50)` — negligible), src/CMRI.cpp:161 (`flush()`).
- The 50 ms stall is gone. `flush()` still blocks until the R frame drains (~9 ms for a SMINI reply at 9600 bps) — correct for RS-485 turnaround (see Strengths) but still a per-poll pause in the sketch's `loop()`.

### [DESIGN-LIMITATION] No double buffering — partially received T data is live
- Status: **still present in v1.7.0**, aggravated by INIT sharing the same buffer (see new BUG).
- Spec: LCS-9.10.1 p.2 (a message is only valid once fully framed).
- Code: src/CMRI.cpp:223 (data bytes written directly into `_rx_buffer` as they arrive, before ETX validates the frame).
- A frame spanning two `loop()` iterations (guaranteed at 9600 bps for a 6-byte SMINI body) lets `get_bit()` observe a half-old/half-new output image. For lights this is a flicker; for paired signal/turnout bits it is a transiently inconsistent state driven onto hardware.

### [DESIGN-LIMITATION] No node-address validation (spec range 0-127)
- Status: **still present in v1.7.0**.
- Spec: LCS-9.10.1 p.3 ("Node address is in the range of 0 to 127 decimal").
- Code: src/CMRI.cpp:29-31, 52-55 — `address` accepted unchecked into a signed `int`; src/CMRI.cpp:190 compares `c == 'A' + _address`.
- An address of 190 yields UA 255 = SYN and can never match (the byte is consumed in preamble states); any address >127 silently produces a node that never responds. No error is reportable.

### [DESIGN-LIMITATION] Unchecked `malloc`, no destructor, dead `MAX` constant
- Status: **still present in v1.7.0**.
- Code: src/CMRI.cpp:35 (`malloc` results never checked; `input_bits = 0` yields `malloc(0)` — benign but implementation-defined); no `free`/destructor (fine for static Arduino usage, leaks if constructed dynamically); src/CMRI.h:51 defines `MAX = 258` which is never referenced — buffer sizing depends entirely on constructor arguments. Cosmetic: the constructor's initializer list order (src/CMRI.cpp:42) does not match declaration order (src/CMRI.h:80-93); initialization follows declaration order so behavior is correct, but `-Wreorder` would flag it.

### [DESIGN-LIMITATION] README API documentation is stale/wrong
- Status: **still present in v1.7.0; drift has grown**.
- Code: README.md:72-78 documents `char process()` returning `NULL`/`CMRI::INIT`/`SET`/`POLL`; the actual API (src/CMRI.h:39, src/CMRI.cpp:65-99) returns `bool`. README.md:75 still says "For INIT requests, it does nothing" — false as of v1.7.0, when `process()` began returning `true` on INIT and the payload landed in the output buffer (a silent behavior change for existing sketches). `set_init_handler()` is documented only in the example sketch, not the README. README.md:120 still describes escaping only 0x03, contradicting the v1.7.0 code that escapes 0x02/0x03/0x10.

## Spec ambiguities encountered

### [SPEC-AMBIGUITY] Stop bits: spec says 1, the installed base (and this library) uses 2
- Spec: LCS-9.10.1 p.2 §A: "CMRInet character framing consists of 10 bits; 1 Start bit, 8 Data bits, 1 Stop bit."
- Code: README.md:45, README.md:103-110, and every example (e.g. examples/hello_world/hello_world.ino, examples/init_handler/init_handler.ino:72) configure `SERIAL_8N2`, with a troubleshooting note that JMRI/classic C/MRI hardware expects 2 stop bits. Unchanged in v1.7.0.
- The spec's 8N1 contradicts three decades of C/MRI practice (Chubb hardware and JMRI both use 8N2). Transmitting 8N2 is received cleanly by an 8N1 receiver (extra stop bit = idle), but an 8N1 transmitter into a strict 8N2 receiver can produce framing errors. The spec is silent on this compatibility question; this library sides with the installed base against the spec's letter — probably the right call, but a master engine must decide explicitly.

### [SPEC-AMBIGUITY] DLE scope excludes the I message — yet I bodies can contain 2/3/16
- Spec: LCS-9.10.1 p.5-6 §D.a mandates DLE insertion only "when forming a Transmit Data or Receive Data message"; p.6-7 §D.1 defines I bodies whose dH/dL/NS/CT bytes may legitimately take the values 2, 3, or 16 (e.g. Table 1 CT values include 2; SMINI CT bitmasks are arbitrary). The spec never says how a byte-stream parser distinguishes a body byte of 0x03 from the message-terminating ETX in an unescaped I body — that requires length-aware parsing driven by NS, which the spec implies but never states.
- Consequence in v1.7.0 — now on BOTH paths: the IGNORE path DLE-unescapes non-addressed traffic including I messages (src/CMRI.cpp:235-244), and since v1.7.0 the *addressed* INIT path does too (src/CMRI.cpp:202-203, 214-233). If a host follows the spec's letter and does NOT escape I bodies: (a) an I to *another* node whose body contains 0x10 right before the real ETX makes this node skip that ETX and swallow the following frame; (b) an I to *this* node containing a raw 0x03-valued dL/NS/CT byte terminates the body early and the remainder is rescanned as noise. If the host (like JMRI) escapes I bodies too, this code is correct and a strictly literal parser elsewhere breaks instead. Either behavior is defensible under the spec; the two choices are mutually incompatible in edge cases. A master engine must pick one (recommend: escape I bodies too, matching JMRI and this library's assumption) and document it.

### [SPEC-AMBIGUITY] No node-side timeout/resync guidance
- Spec: LCS-9.10.1 p.8 §D.2 defines a timeout only for the Host awaiting a Receive Data reply. Nothing specifies how a Node should recover from a frame that never delivers its ETX, nor a maximum inter-byte gap. This library's answer (unchanged in v1.7.0) is "wait forever" (see truncated-frame BUG above); nothing in the spec forbids that, but interoperable recovery behavior is left entirely to the implementer.

## Strengths

### [STRENGTH] Non-addressed traffic is flushed to ETX with DLE honored
- Spec: LCS-9.10.1 p.5 ("If there is no address match, the Node discards all the remaining bytes until an ETX is seen"), p.3 lines 98-100.
- Code: src/CMRI.cpp:210-244 (`IGNORE_CMD`/`IGNORE_DATA`/`IGNORE_ESC_DATA`). Unchanged in v1.7.0; now covered by a unit test (test/test_cmri/test_main.cpp:146-157).
- The ignore path correctly skips DLE-escaped bytes, so an escaped 0x03 inside *another node's* T message does not falsely terminate the flush. Many compact libraries drop straight back to preamble hunting and misparse other nodes' binary payloads; this one gets multi-drop coexistence structurally right.

### [STRENGTH] Receive-side DLE ordering is correct, including escaped byte in the last data position
- Spec: LCS-9.10.1 p.6 ("The receiver, when seeing a DLE in the message body, ignores the DLE and takes the next character in the data stream for processing").
- Code: src/CMRI.cpp:214-233 — ESC is tested *before* ETX, and `DECODE_ESC_DATA` stores the following byte unconditionally, so `... DLE 0x03 ETX` correctly stores 0x03 as data and then terminates on the real ETX.

### [STRENGTH] RX buffer overrun is guarded; TX escaping now spec-complete
- Code: src/CMRI.cpp:219-223, 227-231 — data beyond `_rx_length` is silently discarded, so an oversized or hostile T (or, in v1.7.0, I) frame cannot overflow the receive buffer. As of v1.7.0, src/CMRI.cpp:155-158 escapes all three protocol values (2, 3, 16) on transmit per LCS-9.10.1 §D.a. Truncation remains silent (no diagnostic), but memory-safe.

### [STRENGTH] Correct UA offset both directions; clean non-blocking, composable RX design; INIT payload exposure
- Spec: LCS-9.10.1 p.3, p.5 lines 148-151 (UA = address + 65); p.6 §D.1.
- Code: src/CMRI.cpp:190 (`'A' + _address` on decode) and src/CMRI.cpp:151 (`65 + _address` on the R reply). The byte-at-a-time `_decode()` plus public `process_char()` enables multiple node instances on one port (examples/multiple_nodes/multiple_nodes.ino), and the `Stream&` constructor parameter allows SoftwareSerial/RS-485 transports without library changes. New in v1.7.0: `set_init_handler()` (src/CMRI.h:37, src/CMRI.cpp:57-60) finally exposes NDP/dH/dL/CT to the sketch — the only implementation path this library offers toward honoring init parameters.

### [STRENGTH] `flush()` after transmit gives correct RS-485 turnaround semantics
- Code: src/CMRI.cpp:161. On Arduino ≥1.0, `Stream::flush()` blocks until outgoing data is transmitted, so when paired with a DE-managing wrapper (Auto485, examples/rs485_rx_and_tx/rs485_rx_and_tx.ino) the driver is not disabled mid-frame. Implementations that omit this routinely clip the last byte(s) of the R reply on RS-485.

### [STRENGTH] (NEW in v1.6.0/v1.7.0) Native unit tests and CI
- Code: test/test_cmri/test_main.cpp (Unity, PlatformIO `native` with a mocked `Stream`), .github/workflows/ci.yml, extras/hardware_test/.
- Tests cover DLE escaping on transmit (including the STX regression), accessor bounds (including the v1.5.1 over-correction), poll→R framing, address filtering with 0xFF payloads, and preamble resync after garbage. This is the only implementation of the four under review known to have host-runnable protocol tests; the mocked-Stream pattern is directly reusable for a master engine's test rig. (Gap: no test for triple-SYN, truncated frames, or INIT/output-buffer isolation — exactly where the remaining bugs live.)

## Master-library implications
Lessons for building a CMRInet HOST/master engine:
- **Per-node timeout budgets must span library versions.** v1.5 nodes reply after a fixed 50 ms; v1.5.1+ nodes reply in ~50 µs plus frame time. Both are deployed. Make the poll timeout per-node configurable with a default ≥60 ms, and consider auto-tuning down for fast responders.
- **Send exactly two SYNs.** This parser (all versions through v1.7.0) drops frames preceded by three or more 0xFF bytes. A master must transmit precisely `FF FF 02` — never pad with extra SYN/idle bytes before STX.
- **Expect very fast replies that may overlap your ETX.** v1.5.1+ nodes start their R reply ~50 µs after the `P` byte — before the master's ETX has finished transmitting. On 4-wire this is harmless; if a 2-wire profile is ever supported, the master must drop its driver immediately after ETX and tolerate a reply whose first SYN arrives during its own last character.
- **Tolerate unescaped 0x02 in R bodies from old nodes.** Fixed in v1.7.0, but ≤v1.6.0 nodes in the field still send raw 0x02. The master's R parser must treat only an unescaped 0x03 as terminator and attach no meaning to a raw 0x02 mid-body.
- **Escape I-message bodies (2/3/16) despite the spec's T/R-only wording.** v1.7.0 nodes DLE-unescape addressed I bodies; the ignore path in all versions unescapes non-addressed I traffic. Escaping I bodies matches this library's assumption (and JMRI practice) and is the interoperable choice; document it as a deliberate deviation from LCS-9.10.1 §D.a's letter.
- **Follow every I message immediately with a full T message.** v1.7.0 nodes clobber their output image with the I payload; an immediate T restores correct outputs and shrinks the glitch window to a few frame times. This is cheap insurance for all node types.
- **Treat a missed poll response as retryable, not fatal.** Truncated-frame recovery is undefined node-side (spec gap) and this library can swallow a following frame after a dangling DLE; one retry masks an entire class of node parser quirks.
- **8N2, not 8N1.** Despite the spec's "1 Stop bit" (p.2 §A), the ecosystem (this library, JMRI, classic Chubb hardware) is 8N2. The master should default to 8N2 and offer 8N1 as an option.
- **Reuse the ignore-path idea and the mocked-Stream test rig.** The DLE-aware flush-to-ETX for non-addressed traffic (and, in a master, for unexpected/late replies) is the best structural idea here; v1.7.0's Unity/native test harness with a fake `Stream` is the best process idea, and both are cheap to adopt.
- **Avoid**: live (non-double-buffered) data application, shared buffers across message types (the v1.7.0 INIT clobber shows how a correct-looking fix regresses), missing inter-byte timeouts, and signed/one-sided bounds checks — all concretely demonstrated failure sources in this codebase's history.
