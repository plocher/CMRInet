# cpCMRI (John's Enhanced) — adversarial CMRInet spec review

## Scope & sources
- Spec: NMRA LCS-9.10.1 "CMRInet Protocol" v1.1, December 2014 (9 pages), read in full from `CMRInet/docs/lcs-9.10.1_cmrinet_v1.1.pdf`.
- Implementation: `/Users/jplocher/Dropbox/Arduino/libraries/cpCMRI`
  - `src/cpCMRI.h` (398 lines), `src/cpCMRI.cpp` (657 lines)
  - `README.md`, `docs/CMRInetProtocol.md` (392 lines), `library.properties` (v0.0.3)
  - Examples: all 9 sketches inspected for serial config; `cpNode_BBLeo_8IN8OUT.ino` read in full.
- Important provenance fact: the on-disk `src/` is an **uncommitted work-in-progress refactor** on top of git tag `checkpoint1` ("tested on real hardware & JMRI"). `git diff` (read-only) shows the committed, hardware-tested version used a blocking-with-timeout `readByte()` that consumed bytes via `_serial.read()`; the working tree replaces it with a 64-byte ring buffer whose reader **never consumes** (see Findings 1–2). Several of the most severe findings below are regressions introduced by this unfinished refactor, not present in the tested tag.
- The `.graffle` state-machine diagram at repo root was not readable as text; the equivalent documented state machine in `docs/CMRInetProtocol.md` (lines 294–390) was used for the doc-vs-code comparison.

## Architecture summary
Node-side only (no host engine). `CMRI_Node::protocol_handler()` is a **resumable, non-blocking, per-byte state machine** (`RESET, SYNC, HEADER, ADDRESS, PTYPE, BODY, ESCAPE`, `cpCMRI.h:308`) intended to be called from `loop()`. State is held in member variables, so partial packets survive across calls — no blocking waits mid-packet (a genuine improvement over the blocking parser documented in `docs/CMRInetProtocol.md:341-390`). Bytes flow `_serial` → `process()` → 64-byte ring buffer → `readByte()` → state machine. Complete packets are dispatched to user callbacks (`initHandler/inputHandler/outputHandler/errorHandler`); POLL responses are synthesized and sent inline from the parser. `send_packet()` writes the frame directly to the Stream. A separate `ioMap`/`ioEntry` layer maps packet bits to built-in pins, I2C expanders, and sketch memory variables.

## Findings

### 1. [BUG] Receiver never consumes bytes — parser livelocks (uncommitted regression)
- Spec: LCS-9.10.1 p.3 (line 94-96) permits state-machine parsing, but any parser must make progress per byte.
- Code: `src/cpCMRI.cpp:39-42` (`readByte()` = `process(); return peek();`), loop at `cpCMRI.cpp:60-61`; `get()` (`cpCMRI.h:189-194`) is defined but never called anywhere in the library or examples (verified by grep).
- `peek()` is non-destructive, so `while (peek() != -1)` never becomes false once one byte arrives, and `c` is the same byte on every iteration. First byte of any traffic → infinite loop inside `protocol_handler()`: hang on AVR, watchdog reset on ESP32/ESP8266. The library as it sits on disk cannot pass a single packet.
- Failure scenario: power up, host sends anything; node freezes on the first byte.
- Fix: `readByte()` must call `get()`. The committed `checkpoint1` version consumed via `_serial.read()` and worked; this is an unfinished refactor.

### 2. [BUG] Ring buffer off-by-one: writer and reader use different slot conventions
- Code: `put()` writes at `next(_head)` (`cpCMRI.h:172-178`) while `peek()`/`get()` read at `_tail` (`cpCMRI.h:183-194`), with empty defined as `head == tail`.
- Trace from reset (head=tail=0): first `put(b0)` stores at `_buffer[1]`; first `get()` returns `_buffer[0]` — a byte that was **never written** (uninitialized garbage). Thereafter every read lags one slot: byte N is only readable after byte N+1 has been `put()`.
- Consequence even after Finding 1 is fixed: the ETX of every packet is stuck in the buffer until the *next* packet's first SYN arrives. A POLL's ETX would not be processed until the host transmits again — but the host is waiting (spec p.8, lines 244-247, timeout interval) for our response. Every poll times out; the R response goes out one host-message late and collides with the host's next transmission on the half-duplex pair (spec p.4 §C).
- Fix: write at `_head` then advance, or read at `next(_tail)`; make conventions match and add a unit test with a mock Stream.

### 3. [BUG][SPEC-DEVIATION] No DLE escaping on transmit — R responses violate mandatory DLE processing
- Spec: p.5-6 §D.a lines 166-171 — "Protocol management software **must** insert a DLE in front of any of these data values [2, 3, 16] when forming a Transmit Data or Receive Data message"; reiterated for R messages p.8 §3 lines 257-258. The library's own docs agree (`docs/CMRInetProtocol.md:264-270`).
- Code: `send_packet()` at `src/cpCMRI.cpp:227-232` writes body bytes raw — no DLE insertion anywhere in the library.
- Failure scenario: a node with 16+ inputs where the first input byte happens to equal 0x02/0x03/0x10 (e.g. exactly bit 1 set → 0x02). The host receiver sees a bare 0x03 as ETX, truncates the R message, and the remaining real bytes (including the true ETX) are parsed as garbage — corrupted input states and/or host framing errors. This bug exists in the committed, "tested with JMRI" version too; it survives testing only for input patterns that avoid 2/3/16.
- Fix: in the body write loop, emit `DLE` before any body byte in {0x02, 0x03, 0x10}. (Do not escape 0xFF — see Finding 12.)

### 4. [BUG] TXEN dropped before UART drains — truncated responses on explicit-TXEN RS-485
- Spec: p.4 §C lines 117-120 (half-duplex RS-422/485); the node must hold its driver enabled for the entire frame.
- Code: `src/cpCMRI.cpp:218-236` — `digitalWrite(_txen_pin, 1)`, then buffered `_serial.write()` calls, then `digitalWrite(_txen_pin, 0)` immediately. Arduino `write()` is asynchronous into the TX FIFO; at 9600 baud a 10-byte R frame is still ~10 ms from the wire when TXEN drops. No `_serial.flush()` before deassert.
- Failure scenario: any board using `set_txen_pin()` (i.e. not the cpNode's 555-timer auto-TXEN, `cpCMRI.h:256-259`) transmits only the first byte or two of every response; the rest is cut off when the driver tri-states. Works on original cpNode hardware (hardware TXEN), fails on generic MAX485 wiring — a trap that testing on cpNode hardware cannot catch.
- Fix: call `_serial.flush()` between the ETX write and TXEN deassert. Also note `set_txen_pin()` never calls `pinMode(pin, OUTPUT)`.

### 5. [BUG] Uninitialized members: `_tx_delay`, `_syn_loops`, `_body_length`, `_ptype`, `_paddr`; initial state is SYNC, not RESET
- Code: constructor initializer list `src/cpCMRI.h:100-113` omits all five; `_state` starts at `SYNC`, so the RESET clause (`cpCMRI.cpp:67-72`) that zeroes `_syn_loops`/`_body_length` has not run for the first packet.
- Worst case is `_tx_delay`: `send_packet()` unconditionally evaluates `if (_tx_delay) delayMicroseconds(_tx_delay * 10)` (`cpCMRI.cpp:215-217`). If the host polls before it inits (or the user installs an `initHandler`, which bypasses the default dH/dL capture at `cpCMRI.cpp:150-152`), the node blocks for a garbage-valued delay — on ESP32 (32-bit arg) potentially minutes, guaranteeing host timeout and appearing as a dead node.
- Secondary: garbage `_syn_loops`/`_body_length` make first-packet parsing nondeterministic (a stale `_body_length < 256` is caught by the guard, but the first assembled packet can carry phantom leading body bytes; a garbage `_syn_loops >= 2` lets the very first non-SYN byte be tried as STX).
- Fix: initialize all members; start `_state` at `RESET`.

### 6. [SPEC-DEVIATION] DLE unescaping is applied to ALL message bodies, including I messages
- Spec: p.5-6 §D.a lines 166-169 mandates DLE processing only "when forming a Transmit Data or Receive Data message". The I-message definition (p.6 §1) says nothing about DLE.
- Code: the `ESCAPE` state (`src/cpCMRI.cpp:120-140`) is applied uniformly in `BODY` regardless of `_ptype`.
- Interop consequence, both directions: (a) if the host does *not* escape I bodies (a strict spec reading), an I message with dL=16 (`DLE`) will swallow the following NS byte, and NS=3 or CT=3 (`ETX` — but see Finding 13: CT=2 (`OXXX`) and dL/NS of 2/3 are legal values) will be mis-framed; (b) if the host *does* escape I bodies (as JMRI-family hosts that reuse their T-message packer tend to do), this parser handles it correctly. So the choice made here matches the dominant host behavior but is formally beyond spec. Unverified assumption flagged: I did not confirm JMRI's exact I-message escaping behavior from JMRI source.

### 7. [SPEC-DEVIATION][DESIGN-LIMITATION] I-message body is not parsed: NDP, NS, CT() ignored; no NDP validation
- Spec: p.6 §1 lines 192-231 — I message carries `<NDP> <dH><dL> <NS> <CT(1)..CT(NS)>`; NDP ∈ {N, X, M, C} selects node semantics.
- Code: default init handling (`src/cpCMRI.cpp:145-153`) reads only `body[1]`/`body[2]` (dH/dL — indices are correct, NDP is `body[0]`). NDP is never checked; NS/CT are never read; an I message with NDP='M' (SMINI) or a truncated body is silently accepted. (Truncation is memory-safe: `CMRI_Packet::set()` zero-fills via `clear()`, `cpCMRI.h:65-80`, so missing dH/dL read as 0.)
- For a cpNode-class ('C') device ignoring NS/CT is defensible (the library's own doc says C nodes set NS=0, `docs/CMRInetProtocol.md:211`), but a robust node should at least reject/flag an NDP it does not implement, since host and node otherwise disagree about I/O geometry with no error indication.

### 8. [BUG][DESIGN-LIMITATION] `errorHandler` never fires on actual parse errors; fires only on nonexistent wire type 'E'
- Code: parse failures (no STX after SYNs: `src/cpCMRI.cpp:91-98`; body overflow: `cpCMRI.cpp:121-129, 193-201`) return `CMRI_Packet::ERROR` **without** updating `_packet` or invoking `errorHandler`. The `errorHandler` dispatch (`cpCMRI.cpp:176-180`) triggers only when a completed packet's MT byte is literally 'E' (0x45) — a type no CMRInet host sends (spec p.5 lines 153-155 defines only I/P/R/T). Additionally that dispatch has no address check, unlike I/P/T.
- Consequence: the example sketches' `errorHandler` (`examples/cpNode_BBLeo_8IN8OUT.ino:144-146,167`) is dead code; real framing errors are invisible unless the caller inspects the return value, and even then `_packet` holds stale contents. No error counters/diagnostics exist.

### 9. [DESIGN-LIMITATION] No inter-character timeout: an aborted frame poisons the next frame
- Spec: p.2 lines 65-67 — bit sequences not meeting the format are not valid messages; spec is silent on recovery timing (see Ambiguities).
- Code: `_state` persists indefinitely (`src/cpCMRI.cpp:54-207`). If a frame dies mid-body (host reset, cable glitch), the parser sits in `BODY` and appends the *next* frame's `SYN SYN STX <UA> <MT>` bytes as body data until it sees that frame's ETX.
- Concrete failure: host aborts a T frame to node 5 after the header; next frame is a T to node 5. The parser terminates the *first* packet at the *second* frame's ETX and delivers a T packet to `outputHandler` whose body includes 0xFF 0xFF 0x02 0x46 0x54 + the real output bytes — outputs (signals, turnouts) driven with garbage. An inter-character timeout (e.g. >2 character times resets to RESET) would prevent this; the committed `checkpoint1` `readByte()` had a 3 ms timeout but even it only returned NOOP without resetting `_state`.

### 10. [SPEC-DEVIATION] Non-addressed traffic is fully parsed rather than flushed to ETX — but this is the more correct behavior
- Spec: p.5 lines 157-161 — "If there is no address match, the Node discards all the remaining bytes until an ETX is seen."
- Code: address is checked only at dispatch (`src/cpCMRI.cpp:146,156,170`); the whole frame, including DLE processing, is parsed for every node.
- This deviation is functionally superior to a naive flush-to-ETX: a DLE-blind flusher stops early at the escaped 0x03 of a `DLE 0x03` sequence inside someone else's T body and then treats the frame's tail as inter-frame garbage, while this full parser stays in sync (see Ambiguity 3). Cost: CPU and the shared 256-byte body buffer are spent on other nodes' traffic; on a busy 115200 network an AVR node burns most of its loop budget parsing frames it will discard (each byte also pays the `TRACE` overhead, Finding 15).

### 11. [SPEC-DEVIATION] All examples configure 8N2; spec mandates 1 stop bit
- Spec: p.2 line 55 — "1 Start bit, 8 Data bits, 1 Stop bit". The library's own doc agrees ("81N", `docs/CMRInetProtocol.md:39`).
- Code: every example uses `SERIAL_8N2` (`examples/*/*.ino`, e.g. `cpNode_BBLeo_8IN8OUT.ino:158`, `cpNode_ProMini.ino:150`, `cpNode_Wemos.ino:129`).
- Practical effect is benign-to-helpful: transmitting 8N2 inserts one extra idle bit (a spec-8N1 receiver sees it as idle line); receiving 8N2 against an 8N1 sender can flag framing errors on back-to-back bytes on some UARTs, though the stop-bit check usually samples only the first stop bit. This mirrors long-standing classic-C/MRI field practice (Chubb-era software used 2 stop bits) and is worth recording as an ecosystem-vs-spec mismatch. A master engine should make stop bits configurable and default per peer expectations.

### 12. [SPEC-DEVIATION] Library documentation invents DLE escaping of 0xFF (SYN)
- Spec: p.5 §D.a lines 163-165 — exactly three values are escaped: 2, 3, 16. 0xFF in a body is legal and unescaped.
- Doc: `docs/CMRInetProtocol.md:61-63` claims "Four of those values are protocol control codes... 2 (STX), 3 (ETX), 16 (DLE) and 255 (0xFF)", and line 310-313 claims DLE processing prevents false SYN triggering in bodies — which is only true if 0xFF were escaped (it isn't, per spec). The code escapes nothing (Finding 3), so the doc describes neither the spec nor the code. A node TX that escaped 0xFF would still interop (receivers take the byte after DLE literally), but a *receiver* built assuming bodies never contain raw 0xFF would be wrong: a host-compliant T message with output byte 0xFF arrives unescaped. This parser handles raw 0xFF in BODY correctly (it only treats 0xFF specially in SYNC), so the code is fine; the doc is the hazard if used as a reference for the master library.

### 13. [SPEC-AMBIGUITY] I-message bodies can contain protocol-character values, and the spec provides no escaping rule for them
- Spec: DLE processing is mandated only for T and R (p.6 lines 166-169), yet the I body legitimately contains: dL=2/3/16 (any delay value), NS=2 or 3, and CT values from Table 1 including 2 (`OXXX`), 1, 10, 6... (p.7 Table 1). A pure state-machine receiver (as the spec itself suggests, p.3 lines 94-96) cannot distinguish an ETX-valued NS byte from end-of-message without out-of-band length knowledge — which the receiver does not have before parsing the I message itself.
- This is a genuine self-inconsistency in LCS-9.10.1: the I message is not reliably parseable under the spec's own framing rules. Real-world hosts resolve it by escaping I bodies like T bodies (which this library's receiver, Finding 6, happens to accept). The master library must pick a policy (recommend: escape on TX for all message types, accept both on RX) and document it.

### 14. [SPEC-AMBIGUITY] No recovery/timeout semantics for malformed or truncated frames
- Spec: p.2 lines 65-67 defines invalid sequences as "not a valid message" and p.8 lines 244-247 gives the Host a poll timeout, but is silent on: how a Node resynchronizes after a truncated frame, whether SYN hunting may occur mid-body, and any inter-character timeout. Finding 9's failure mode is therefore spec-legal behavior. Any robust implementation must invent recovery rules (inter-character timeout, max frame time); the master engine should specify them explicitly.

### 15. [BUG] Library ships with per-byte debug tracing enabled on `Serial`
- Code: `src/cpCMRI.cpp:23` — `#define CMRI_DEBUG (CMRI_DEBUG_PROTOCOL | CMRI_DEBUG_SERIAL | CMRI_DEBUG_IOMAP | CMRI_DEBUG_IO)` is active (the `(0)` variant is commented out). Every state transition prints to `Serial` (`cpCMRI.cpp:68,75,90,...`).
- Two failure modes: (a) on ProMini and Wemos examples the **CMRI link itself is `Serial`** (`cpNode_ProMini.ino:150`, `cpNode_Wemos.ino:129`) — debug text is injected into the CMRInet stream, corrupting the network (mitigated only by the sketch-level `DEBUG 0` comment hint, which does not affect the library's own `CMRI_DEBUG`); (b) even on Leonardo (debug on USB, CMRI on Serial1), printing several lines per received byte at 115200 CMRI speed makes the parser slower than the wire, overflowing the 64-byte ring (Finding 16) and dropping bytes. Debug must default off in a release.

### 16. [DESIGN-LIMITATION] 64-byte ring + 64-byte HW FIFO vs 256-byte bodies: silent overflow between calls
- Spec: p.2 line 75 — bodies up to 256 bytes; at 115200 a max T frame (262+ bytes with escapes) arrives in ~23 ms.
- Code: `_buffer[64]` (`src/cpCMRI.h:326`, effective capacity 63), `process()` stops filling when full (`cpCMRI.h:120-124`) leaving excess to the HW FIFO (typically 64 on AVR), beyond which bytes are silently dropped — no overflow counter, no frame-abort signal. If the sketch spends >~10 ms in user code (I2C transactions in `ioMap` easily do) during a large T frame, bytes vanish mid-body and Finding 9's poisoning follows.
- Mitigation exists in-design (call `process()` from user code often, per `cpCMRI.h:115-118`) but nothing enforces or diagnoses it.

### 17. [BUG] `ioMap::initialize()` uses bitwise-OR instead of AND in flag tests — initial output states and pullup selection are wrong
- Code: `src/cpCMRI.cpp:414-428` — `if (io->flags | OUTPUT_HIGH)`, `else if (io->flags | OUTPUT_LOW)`, `if (io->flags | INVERT)`, `if (io->flags | INPUT_PULLUP)` are all always-true. Net effect: `val` is always computed as `!1 = 0` and every input gets `INPUT_PULLUP` regardless of the table. (The TRACE prints at `cpCMRI.cpp:437-439` use `&` correctly, so the debug output *lies about what the code does*.)
- Failure: `OUTPUT_HIGH` initializations in every example's iomap table (e.g. `cpNode_BBLeo_8IN8OUT.ino:86-88`) are ignored; outputs come up 0 until the first T packet — on a layout, signals/relays in the wrong state after node reset. Non-pullup inputs (`cpNode_BBLeo_8IN8OUT.ino:73` uses plain `INPUT`) get pullups anyway.
- Not protocol, but directly undermines the spec's init contract (node in a defined state after INIT, `docs/CMRInetProtocol.md:91-95`).

### 18. [BUG] MEM8/MEM16 memory-mapped bits are accessed through `*(bool *)` — high bits broken, type-pun UB
- Code: `src/cpCMRI.cpp:262-263` (`bitRead(*(bool *)(expander), pin)`) and `cpCMRI.cpp:301-303` (`bitWrite(*(bool *)(expander), pin, val)`), with `pin` clamped up to 15 for MEM16 (`cpCMRI.cpp:251-253`).
- A `bool`/single-byte lvalue cannot address bits 8–15 of a `uint16_t` sketch variable: reads of pins 8-15 return 0, writes modify only the low byte (and `bitWrite` on a bool lvalue collapses to 0/1). The README-advertised animation/computed-value feature (`README.md:26`) is broken for 16-bit variables and formally UB for 8-bit ones.

### 19. [DESIGN-LIMITATION] `_tx_delay` honored with blocking `delayMicroseconds()`, out of range on AVR
- Spec: p.6 lines 203-206 — dH/dL up to 65535 units × 10 µs = 655.35 ms.
- Code: `src/cpCMRI.cpp:215-217`. AVR `delayMicroseconds()` is documented accurate only to 16383 µs; larger requested delays wrap/misbehave. Also fully blocking inside the parse loop. Low practical severity (spec: "set to zero for modern Host computers") but a master engine must not copy this pattern.

### 20. [DESIGN-LIMITATION] Minor items (grouped)
- Address never range-checked against 0–127 (spec p.5 line 148): `_paddr = byte(c) - 'A'` (`src/cpCMRI.cpp:103`) accepts UA bytes below 65 (yielding negative addresses — harmless, never match) and above 192; `send_packet()`'s `'A' + packet.address()` (`cpCMRI.cpp:225`) can wrap for out-of-range configured addresses. Comments/docs say address range "0..64" (`cpCMRI.h:97,235`, example line 13) vs spec's 0–127 — cpNode convention presented as protocol fact.
- Dead code: `if (_syn_loops <= 1) break;` at `src/cpCMRI.cpp:84` is unreachable (the condition is subsumed by line 76). Harmless but obscures the SYNC logic.
- `CMRI_Packet` header comment claims "as a Stream subclass, it can be used as a mock Serial source" (`src/cpCMRI.h:15-18`) — it is not a Stream subclass; no mock exists.
- POLL body is not validated as empty (spec p.7 §2); any body on a P frame is silently accepted.
- If no `inputHandler` is set, a POLL yields a zero-length R (`src/cpCMRI.cpp:163-166`) — a host expecting NI bytes will flag a short-reply error; defensible default, worth documenting.
- The implemented state machine does not match the one documented in `docs/CMRInetProtocol.md:341-390` (which is blocking, has no ADDRESS/PTYPE/ESCAPE states, and checks overflow at `idx > 255` allowing 257 bytes). The doc describes an older design.
- RAM: `CMRI_Node` carries both `_pbody[257]` and `_packet._body[256]` plus the 64-byte ring ≈ 580 bytes — over 25% of an ATmega328's SRAM before the sketch starts; the double body copy (`_packet.set()` at `cpCMRI.cpp:143` copies `_pbody` → `_body`) is avoidable.

## Spec ambiguities encountered
1. **I-message bodies vs framing** (Finding 13): DLE escaping is mandated only for T/R, yet I bodies legally contain 2/3/16 — the I message is not unambiguously parseable under the spec's own rules. LCS-9.10.1 p.6 lines 166-169 vs p.6-7 §1 + Table 1.
2. **No recovery semantics** (Finding 14): nothing specifies node resynchronization after truncated/garbled frames, inter-character timeouts, or maximum frame duration. p.2 lines 65-67, p.8 lines 244-247.
3. **"Discard until ETX" for non-addressed nodes** (Finding 10): taken literally (p.5 lines 157-161), a DLE-blind flush stops at an escaped 0x03 inside someone else's T body. The spec's discard rule interacts unsafely with its own DLE rule unless the flusher is DLE-aware — which the spec doesn't say.
4. **Stop bits vs field practice** (Finding 11): spec says 1 stop bit (p.2 line 55); the classic C/MRI ecosystem this spec exists to standardize commonly runs 8N2, and every example in this library does. The spec is silent on tolerance/interop.
5. **UA range vs node types**: spec allows addresses 0–127 (p.3, p.5 line 148) but classic node families and this library's docs use 0–63/64; no guidance on what a host should do with addresses above a node family's limit.

## Strengths
- **Resumable non-blocking parser** (`src/cpCMRI.cpp:54-208`): state held in members, returns NOOP when starved, never busy-waits for a byte mid-frame (contrast the doc's own blocking example and typical `while(!available())` node implementations). Right architecture for a cooperative `loop()`.
- **Correct DLE-before-ETX ordering** (`cpCMRI.cpp:137-142`): DLE is checked before ETX in BODY, and the dedicated ESCAPE state handles an escaped 2/3/16 in the final body position and across call boundaries correctly — a spot many implementations get wrong.
- **Full 256-byte body support with overflow guards before every store** (`cpCMRI.cpp:121-129, 193-201`; `_pbody[BODY_MAX+1]`, `cpCMRI.h:331`): matches spec p.2 line 75 exactly, where many node libraries cap at 64/72 bytes; guarded in both BODY and ESCAPE paths, so an oversized frame yields a clean ERROR, not memory corruption.
- **Two-SYN preamble enforcement with garbage tolerance** (`cpCMRI.cpp:74-87`): arbitrary inter-frame garbage is discarded; ≥2 consecutive SYNs then STX required; extra SYNs tolerated (unbounded count) — matches spec p.3 lines 143-146 and resyncs after ERROR by returning to RESET.
- **Default dH/dL capture** (`cpCMRI.cpp:150-152`): even with no user `initHandler`, the transmission delay from the I message is honored (spec p.6 lines 203-206) — with correct byte order (dH×256+dL).
- **Debug/test affordances**: `b2s()`/`packetToString()` (`cpCMRI.h:267-305`) render frames in CMRI terminology; `CMRI_Packet::set()` builds arbitrary packets (usable for host-side TX and unit tests); per-state TRACE hooks; `docs/CMRInetProtocol.md` adds genuinely useful beyond-spec host guidance (resend INIT periodically, handle node response failures, no assumed POLL/TX ordering — lines 86-117).
- **`ioMap` abstraction** (`cpCMRI.h:338-395`): declarative bit-to-hardware mapping with host-centric vs node-centric packing, arbitrary I/O interleaving, and sketch-variable-backed bits — a clean seam between protocol and hardware that a master library can mirror (the concept, not this implementation — see Findings 17–18).

## Master-library implications
What to reuse:
- The **resumable per-byte state machine with explicit ESCAPE state** is the right receive architecture for the host side too (host must parse R frames): copy the state set (RESET/SYNC/HEADER/ADDRESS/PTYPE/BODY/ESCAPE), the DLE-before-ETX ordering, and the guard-before-store overflow discipline. Add what's missing here: inter-character timeout → RESET, error counters, and parse-error callback.
- `CMRI_Packet` as a value type (type/address/body/length + `set()`/`packetToString()`) is a good host-side building block; make the "mock Stream for testing" comment true by actually providing a loopback Stream — the lack of any unit test is precisely why Findings 1–2 shipped to disk.
- The docs' host guidance (periodic INIT refresh, poll timeout handling, no POLL/TX ordering assumptions) should become explicit master-engine features.

What to avoid / do differently:
- **Escape on TX, always** (Finding 3): the master forms I and T messages — escape 2/3/16 in both (Finding 13 policy: escape all types on TX, tolerate both on RX). Never escape 0xFF (Finding 12's doc error).
- **flush() before TXEN release** (Finding 4), and make TXEN handling own `pinMode`. On the host side the dual is: after sending P, ensure the TX FIFO has drained before starting the response timeout clock.
- **Initialize every member; start in RESET** (Finding 5). Treat delay parameters as untrusted until an I exchange occurs.
- **Define recovery explicitly** (Findings 9/14): inter-character timeout (~2–3 char times), max-frame timer, and a diagnostics surface (framing-error, overflow, timeout counters per node) — the spec won't tell you; decide and document.
- **Size buffers for 256-byte bodies + worst-case escaping** (×2 on the wire) and make overflow loud, not silent (Finding 16).
- **Keep debug output off by default and never on the protocol Stream** (Finding 15).
- Ring buffers and bit-mapping layers need unit tests with a mock Stream before hardware; the off-by-one (Finding 2) and `|`-vs-`&` (Finding 17) classes of bug are trivially caught by tests and nearly invisible in hardware bring-up.
- Validate UA range (0–127) at API intake and on RX; don't propagate the cpNode "0..64" convention into the protocol layer.
