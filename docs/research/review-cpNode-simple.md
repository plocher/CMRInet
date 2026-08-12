# cpNode library ("John's Simple") — adversarial CMRInet spec review

## Scope & sources
- Spec: NMRA LCS-9.10.1 "CMRInet Protocol" v1.1, December 2014 (9 pages). Citations use page and spec line numbers (the printed left-margin numbers).
- Code reviewed (read-only, not compiled):
  - `cpNode/src/cpNode.h` (176 lines)
  - `cpNode/src/cpNode.cpp` (~511 lines); receive state machine `getPacket()` at cpNode.cpp:335-466
  - `cpNode/README.md`
  - Examples: `BBLeo/BBLeo.ino`, `Xiao-I2C-simple/Xiao-I2C-simple.ino`, `Xiao_I2C/Xiao_I2C.ino` (plus a survey of the 16 other sketches for `Serial.begin` configuration)
- Role: node-side (slave) implementation. Reviewed as one of four parallel implementation reviews feeding a future CMRInet HOST/master engine.

## Architecture summary
Single-class (`cpNode`) polled protocol handler driven from the sketch's `loop()` via `process()` (cpNode.cpp:85-109). `getPacket()` is a byte-driven switch parser: non-blocking entry (returns `Packet_None` if no byte available, cpNode.cpp:346-348), but once a byte is present it reads to end-of-frame using `callback_read_CMRI_Byte()` (cpNode.cpp:307-313), a **blocking busy-wait with no timeout**. There is no explicit SYN/STX hunting state: the parser reacts to STX/ETX/DLE/SYN bytes wherever they occur. The message body (post-MT bytes, DLE-unescaped) is stored in `CMRInet_Buf[260]`. Poll responses are built into the same buffer and written byte-at-a-time with optional per-character delay and optional TXEN (RS-485 driver-enable) discipline (cpNode.cpp:225-300). I/O is delegated to sketch-supplied `pack()`/`unpack()` callbacks.

## Findings

### [BUG] `inCnt` never reset when STX is seen — pre-header garbage shifts the message body
Spec: LCS-9.10.1 p.2-3 (message = header/body/trailer; "Any sequence of bits not meeting the full specification ... is not ... a valid message"), p.5 lines 144-146.
Code: cpNode.cpp:353-354 (`inCnt = 0` only at function entry), cpNode.cpp:360-406 (STX case does not reset `inCnt`), cpNode.cpp:428-436 (default case stores bytes into `CMRInet_Buf` **without checking `inData`**).
Any non-protocol byte received before the header (line noise, tail of a mangled previous frame, bytes left over after an early flush exit — see the flush finding) is stored at `CMRInet_Buf[0..]`. When the real `STX <UA> <MT>` then arrives and the frame is addressed to this node, the true body lands at a nonzero offset, but `callback_unpack_Node_Outputs()` (cpNode.cpp:142-147) reads `CMRInet_Buf[0..nOB-1]`. Concrete failure: one noise byte immediately before a valid T frame causes every output byte to be replaced by `<noise, OB(1), OB(2), ...>` — wrong signals/turnouts driven on the layout, silently. The same shift corrupts I-message option parsing (cpNode.cpp:156-161). Note the SYN case *does* check `inData` (cpNode.cpp:424) — the guard was simply omitted from the default and DLE cases.
Fix: reset `inCnt = 0` in the STX case, and only buffer bytes when `inData` is true.

### [BUG] Buffer-overrun guard is off-by-one twice — 2-byte out-of-bounds write into `OB[]`
Spec: LCS-9.10.1 p.2 line 75 (body max 256 bytes).
Code: cpNode.h:66 (`CMRInet_BufSize = 260`, valid indices 0..259), cpNode.cpp:441 (`if (inCnt > CMRInet_BufSize)`), cpNode.cpp:455 (`CMRInet_Buf[inCnt] = 0`).
The check permits `inCnt` to reach 260 (index 259 written) and continue; the next byte writes index 260 (out of bounds) before `inCnt`=261 finally trips the guard. Then line 455 writes the NUL at index 261 — a second OOB write. Even without tripping the guard, a frame that fills exactly 260 stored bytes puts the NUL at index 260. In the class layout `OB[]` immediately follows `CMRInet_Buf` (cpNode.h:140-141), so the overflow corrupts `OB[0..1]` — the next `unpack()` drives wrong outputs. Reachable with a >259-byte unterminated garbage burst, or a legal near-max body combined with buffered pre-header garbage (previous finding).
Fix: guard with `inCnt >= CMRInet_BufSize - 1` *before* each store.

### [BUG] Blocking byte reads with no timeout — truncated frame stalls the node
Spec: LCS-9.10.1 p.8 lines 243-247 describes timeout handling for the Host; it is silent on Node-side timeouts, but a node that stops responding defeats the poll loop.
Code: cpNode.cpp:307-313 (`while (true) { if (available()>0) return ... }`), called from cpNode.cpp:357, 365, 381, 414.
Once `getPacket()` has consumed one byte it will not return until it sees ETX (or overrun). If a frame is truncated (host reset mid-frame, cable glitch eating the ETX, or a DLE as the last byte before silence — cpNode.cpp:414 blocks waiting for the escaped byte), the node busy-waits forever: no polls answered, no outputs updated, until more bus traffic happens to arrive (whose leading bytes are then mis-consumed). On AVR this is a silent permanent hang of the application loop; on the ESP32-C6 target the starved idle task can trip the task watchdog and reboot the node. Fix: add a per-byte timeout (e.g. a few character times at the configured baud rate) that aborts to `Packet_Err`.

### [BUG] No length validation on received T data or configured buffer sizes
Spec: LCS-9.10.1 p.8 lines 266-269 (T carries NO output bytes); the spec has no explicit length field, so length checking is the receiver's job.
Code: cpNode.cpp:139-150 (`callback_unpack_Node_Outputs` copies `nOB` bytes unconditionally); cpNode.h:67 (`IO_bufsize = 22`); cpNode.cpp:242-256 (poll-response fill loop has no bound check); cpNode.cpp:268 (`for (byte j=0; j<i; ...)` with `int i`).
Three consequences:
1. A short or truncated T frame (fewer than `nOB` body bytes) silently writes stale/garbage buffer bytes to the layout — `inCnt` is never compared to `nOB`.
2. `setNumOutputBytes()`/`setNumInputBytes()` accept any byte value with no validation against `IO_bufsize` (22). `nOB > 22` makes `unpack()` copy past `OB[]`; `nIB > 22` makes the poll response read past `IB[]` and transmit garbage.
3. Worst case (`nIB ≥ 128`, all bytes needing DLE) the response fill loop overruns `CMRInet_Buf` (516 > 260) with **no guard at all** on the TX path, and because the write-loop index `j` is a `byte` while `i` is an `int`, `i > 256` makes `j < i` always true — an infinite transmit loop with TXEN asserted, jamming the RS-485 bus.
This contradicts the library README's own claim that the length parameters let code "protect itself" (README.md:64-65). Fix: clamp `nIB`/`nOB` to `IO_bufsize`, verify `inCnt >= nOB` before unpacking, and bound the TX fill loop.

### [SPEC-DEVIATION] Receiver requires DLE escaping inside the I-message body; spec only mandates it for T/R
Spec: LCS-9.10.1 p.5-6 §D.a lines 166-171 ("must insert a DLE ... when forming a Transmit Data or Receive Data message" — I is not listed); p.6 §1 lines 190-215 (I body may legitimately contain raw 2/3/16: `<dL>`=3 is a legal 30 µs delay; CT(OXXX)=2 per Table 1, p.7).
Code: cpNode.cpp:408-421 — the ETX and DLE cases act on those byte values unconditionally once in the body, with no I-specific mode.
A strictly spec-compliant host that does *not* DLE-escape an I body will desync this node: `dL`=0x03 terminates the frame early (options then parsed from a truncated/stale buffer); `dL`=0x02 restarts header parsing mid-body (usually degrades to `Packet_Ignore` + flush); `dL`=0x10 swallows the next byte and drops the 0x10, shifting all following option bytes. In practice JMRI-family hosts escape everything, which masks this; against other masters it is an interop landmine. (The reverse direction is correct: the node only ever *builds* an R message, where escaping is mandatory.)

### [SPEC-DEVIATION] Non-addressed traffic is not atomically flushed to ETX; leftover bytes get re-parsed
Spec: LCS-9.10.1 p.5 lines 157-161 ("If there is no address match, the Node discards all the remaining bytes until an ETX is seen"); p.3 lines 98-100.
Code: cpNode.cpp:374-377 (`Packet_Ignore` returns immediately without consuming to ETX), cpNode.cpp:201-212 (`callback_flush_CMRInet_to_ETX` exits when the RX buffer is merely *empty* — a deliberate v1.4.2 change per README.md:131).
Because the CPU drains the UART far faster than bytes arrive, the flush almost always exits on "empty" long before the ignored message ends. The remainder of another node's message is then re-parsed by the next `getPacket()` call as if it were a fresh stream. Usually this degrades to `Packet_Err` + another flush (and, per the `inCnt` finding, can leave garbage in the buffer). Adversarial case: an unescaped I body addressed to *another* node containing `0x02 <thisUA> 'P'` is interpreted as a poll for this node — this node transmits an R while the host may still be mid-message: a bus collision on the half-duplex pair shared with other nodes' responses. Low probability, but it is a protocol-level spoof enabled by the combination of this deviation and the previous one.
Fix: on `Packet_Ignore`, consume to ETX inside `getPacket()` itself (with the byte-timeout from the blocking-read fix), rather than relying on the opportunistic flush.

### [SPEC-DEVIATION] Node address limited to 0..64 and silently clamped
Spec: LCS-9.10.1 p.3 (UA table: "Node address is in the range of 0 to 127"); p.5 lines 148-151.
Code: cpNode.cpp:69-80 (`if (nodeAddr > 64) nodeAddr = 64;`), cpNode.h:23 (comment "0..64 ('A'..DEL)" — arithmetically wrong: 64+65=129, not DEL=127).
Addresses 65-127 are unusable, and an out-of-range configuration is silently *changed to 64* rather than rejected — two nodes misconfigured this way would both answer address 64. Fix: allow 0..127 per spec, and fail loudly (or at least return an error the sketch must check) instead of clamping.

### [SPEC-AMBIGUITY] I-message body layout for NDP='C' is not defined by the spec; code implements the MRCS/JMRI convention
Spec: LCS-9.10.1 p.6 lines 197-215 and p.7 define NS/CT semantics for N, X, and M nodes only; for C (CPNODE) the spec names the NDP (p.6 line 201) but never defines what NS/CT mean.
Code: cpNode.cpp:155-174 and the comment at cpNode.cpp:181 parse `<NDP><DLH><DLL><opts1><opts2><NIN><NOUT>` at fixed offsets.
The code follows the de-facto cpNode convention rather than the spec's `<NS><CT(1)>...` structure — defensible, since the spec is silent, but a master must know that C-type nodes have a private I-body dialect. Recorded as a genuine hole in LCS-9.10.1.

### [DESIGN-LIMITATION] Init parameters are decoded but mostly ignored; no validation, no rejection path
Spec: LCS-9.10.1 p.6 lines 192-195 ("Each Node ... must have an initialization message sent").
Code: cpNode.cpp:155-174 — `opts1`, `opts2`, `nIN`, `nOUT` are read into locals used only by a debug print; only DL is retained. cpNode.cpp:183-190 — an I with NDP≠'C' is silently ignored (host believes the node initialized).
A JMRI node definition whose byte counts disagree with the sketch's `setNumInputBytes`/`setNumOutputBytes` is never detected (README.md:55-56 admits "there is no runtime validation"). Mis-framed data then flows silently every poll cycle. A master engine should assume C nodes never NAK a bad init.

### [SPEC-AMBIGUITY] Transmission-delay semantics: per character or per message?
Spec: LCS-9.10.1 p.6 lines 203-206 — "Each unit of transmission delay represents 10 microseconds", "will cause SMINI, SUSIC and USIC Nodes to delay **between transmissions** to the Host". It does not say between *characters*, and conspicuously omits CPNODE from the list of node types that honor it.
Code: cpNode.cpp:163-167 computes DL in µs; cpNode.cpp:268-275 applies `delayMicroseconds(DL)` after **every transmitted character** of the R response (including the final ETX, delaying TXEN release).
The classic Chubb implementations used DL as an inter-character pacing delay, and this code follows that tradition, but the spec text as written is ambiguous (and arguably exempts cpNodes entirely). A master must not assume either interpretation when computing poll timeouts.

### [BUG] `delayMicroseconds()` overflow for large DL on AVR
Spec: LCS-9.10.1 p.6 lines 203-204 (dH/dL is 16-bit → up to 655,350 µs per unit-delay).
Code: cpNode.cpp:272-274.
AVR `delayMicroseconds()` takes an `unsigned int` and is only accurate to 16383 µs; DL up to 655,350 is truncated mod 65536 and produces wrong (much shorter) delays. Edge case — hosts normally send 0 — but the honored range silently differs from the accepted range. Fix: use `delay(DL/1000); delayMicroseconds(DL%1000);` if large DL must be honored.

### [SPEC-DEVIATION] Stop-bit configuration inconsistent with spec and across examples
Spec: LCS-9.10.1 p.2 line 55 — "1 Start bit, 8 Data bits, 1 Stop bit".
Code: the library never configures the port; sketches own `Serial.begin`. `Xiao-I2C-simple.ino:169` and `Xiao_I2C.ino:227` use `SERIAL_8N2`; all AVR examples (e.g. `BBLeo.ino:42`) use the default 8N1.
8N2 transmit is compatible with 8N1 receivers (extra stop bit = idle), so this rarely breaks anything, but it deviates from the spec's character framing and the library's own examples disagree with each other. Note: much of the historical C/MRI ecosystem (including, I believe, JMRI's default CMRI serial settings) uses two stop bits — I did not verify JMRI sources in this review, but the tension between spec text and ecosystem practice is worth recording (see Spec ambiguities).

### [BUG] Debug print can overflow `debug_buffer[128]`
Code: cpNode.cpp:282-298 (header sprintf ≈45 chars, then `strcat` of ~5-11 chars per data byte), cpNode.h:129 (`char debug_buffer[128]`).
With `DEBUG_PROTOCOL` enabled and ≥ ~16 input bytes (a 16IN + IOX configuration), the assembled line exceeds 128 bytes and `strcat` smashes the adjacent class members (`invert_in`, `UA`, ...). Debug-only, but it corrupts the node's own address field, which then makes it stop answering polls — a confusing failure mode precisely when someone is debugging. Fix: use `snprintf` with remaining-space accounting, or print items directly.

### [BUG] Addressed 'R' frames trigger a flush that can eat the next frame
Spec: LCS-9.10.1 p.8 lines 249-251 (R is NODE to HOST only).
Code: cpNode.cpp:389-390 (`Packet_Read` accepted as a valid inbound type; frame fully consumed to ETX), cpNode.cpp:103-106 (`process()` has no `Packet_Read` case → default → flush-to-ETX).
Since the R frame was already consumed, the flush consumes whatever comes next — potentially the header of a following valid frame. On a correctly wired four-wire network a node never hears an R addressed to itself, so this is latent (echoing converters or two-wire 485 wiring would expose it). Fix: treat inbound 'R' as `Packet_Err` without flushing past the already-consumed ETX.

### [DESIGN-LIMITATION] Inputs sampled only at poll time; no latching or debounce despite comments
Code: cpNode.cpp:111-123 — the author's own comment honestly notes the debounce comment "does not match the actual code"; cpNode.cpp:124-132, cpNode.cpp:254 (`IB[j] = 0; // Clear the latched inputs` — nothing is actually latched; `pack()` overwrites IB on the next poll anyway).
Input pulses shorter than the host's poll interval are lost. Classic SMINI firmware latches input changes between polls. Masters should poll fast and not assume node-side latching for C-type nodes built on this library.

### [DESIGN-LIMITATION] Whole-byte inversion includes unused bit positions
Code: cpNode.cpp:127-131 (`IB[i] = ~IB[i]` across all `nIB` bytes), e.g. `BBLeo.ino:105-118` where `IB[0]` uses only 6 bits and `IB[1]` uses none.
With `invertInputs(true)`, every unused/padding bit is reported to the host as 1 (active). Host-side tables that accidentally reference an unused bit see a permanently-active sensor. Also note the inversion stack is three layers deep in the flagship example (sketch-level `!digitalRead`, MCP23017 IPOL active-low registers at cpNode.cpp:481-484, plus library `invert_in`) — consistent in `BBLeo.ino` as written, but very easy for a user to get one layer wrong with no way to detect it.

## Spec ambiguities encountered
- **C-NDP init body undefined** (p.6 lines 197-215): the spec introduces NDP 'C' but defines NS/CT semantics only for N/X/M. Every cpNode-family implementation necessarily invents (or inherits) a private layout. The spec should either define the C body or state that it is implementation-defined.
- **Transmission delay granularity** (p.6 lines 203-206): "delay between transmissions to the Host" — per character or per message? And is CPNODE required to honor it at all, given only SMINI/SUSIC/USIC are named?
- **Body length "0 to 256 data bytes"** (p.2 line 75): pre- or post-DLE-stuffing? A 256-byte logical body can be up to 512 bytes on the wire. Receive buffers sized to "256" on wire-byte counting would be wrong; this library counts unescaped bytes (correct under the most useful reading), but the spec never says.
- **Stop bits** (p.2 line 55): spec mandates 1 stop bit, while long-standing C/MRI practice (classic firmware and, per my recollection — unverified here — JMRI defaults) uses 2. A master engine must make this configurable.
- **Node-side error behavior**: the spec defines Host timeout handling (p.8 lines 243-247) but says nothing about what a Node should do with unknown MTs, malformed frames, or missing ETX. Every implementation invents its own recovery, which is exactly where this library's bugs cluster.
- **SYN preamble optionality** (p.3 lines 143-146 say a message "begins with two bytes of all ones... to synchronize the hardware receivers"): must a receiver *require* the SYNs? This implementation ignores them entirely (tolerant); the spec doesn't say whether that is conformant.

## Strengths
- **[STRENGTH] DLE unescape is ordered correctly ahead of ETX/STX interpretation** (cpNode.cpp:413-421 vs spec p.5-6 §D.a): the DLE case blindly takes the next byte, so an escaped 2/3/16 works in any body position *including the last byte before ETX* — a classic failure point in other implementations.
- **[STRENGTH] 0xFF handled per spec on both sides**: SYN bytes inside a body are accepted as data once `inData` is set (cpNode.cpp:423-426), and the transmitter correctly does *not* escape 0xFF (the commented-out `case SYN:` at cpNode.cpp:246 shows this was a considered decision). Matches the spec exactly (only 2/3/16 are escaped).
- **[STRENGTH] Correct RS-485 TXEN discipline** (cpNode.cpp:264-279): assert TXEN → write → `flush()` → deassert. Both the AVR and ESP32 Arduino cores' `flush()` wait for the full TX drain (including the shift register), so the R message's final ETX is never clipped by an early driver-disable — a common real-world 485 bug.
- **[STRENGTH] Non-blocking entry and honest self-documentation**: `process()` returns immediately when the bus is idle (cpNode.cpp:346-348); the author explicitly annotates places where comments and code disagree (cpNode.cpp:115-119) rather than leaving misleading comments.
- **[STRENGTH] Buffer sized for the real maximum** (cpNode.h:66: 260 > 256-byte max body, storing unescaped bytes), and overrun is at least *detected* with a diagnostic and a flush-based resync (cpNode.cpp:441-449) — the guard is off-by-one (see Findings) but the recovery strategy is right.
- **[STRENGTH] Signedness-consistent protocol-character comparisons**: `SYN` and the RX byte both travel through `char`, so the SYN case matches on AVR (signed char, both −1) and on RISC-V/ESP32-C6 (unsigned char, both 255) alike. Fragile by construction, but not currently broken on either target.

## Master-library implications
What to reuse:
- The DLE ordering model: unescape *before* interpreting ETX/STX, take-next-byte-literally. Also copy the decision matrix exactly: escape only 2/3/16, never 0xFF, and (for wire-compat with this node) escape T-message bodies but *also* escape I-message bodies — this node's parser requires it, and it costs nothing.
- The TXEN pattern (assert → write → flush-to-drain → deassert) for any master that drives a half-duplex converter with explicit direction control.
- Storing *unescaped* body bytes and sizing buffers at 256 + slack.

What to avoid (design the master defensively around these node behaviors):
- **Never send an I body with unescaped 2/3/16** — spec permits it, this node breaks on it.
- **Assume nodes may hang or desync on truncated frames**: the master must have hard per-node poll timeouts (spec p.8 lines 243-247) and must treat a silent node as recoverable, ideally re-sending init after repeated timeouts.
- **Precede every frame with the full SYN SYN preamble and never rely on inter-frame gaps for node resync**: this node resyncs only by hunting ETX, so a clean, gapless, fully-framed byte stream is the best way to keep it (and its siblings) aligned. Avoid emitting any bytes between frames.
- **Always send exactly `nOB` bytes in T messages** — this node applies no length checking and will happily unpack stale buffer contents from a short frame.
- **Address range**: masters should support UA 0-127 per spec, but be aware fielded cpNodes cap at 64.
- **State-machine hygiene lessons** for the master's own receiver: reset the body index on STX, guard every buffer store *before* writing, bound-check the null terminator, and never busy-wait on a byte without a timeout — each of these is a concrete defect found here.
- **Config validation**: the master should verify polled R-message lengths against its node table and raise a diagnostic on mismatch, because C-type nodes (this library) will never report a misconfiguration themselves.
