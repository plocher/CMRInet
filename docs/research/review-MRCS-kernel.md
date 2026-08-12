# MRCS_cpNode_kernel — adversarial CMRInet spec review

## Scope & sources
- Spec: NMRA LCS-9.10.1 "Communications Specification for CMRInet" v1.1, Dec 2014 (9 pp). Cited as "p.N §X (line L)" using the spec's printed line numbers.
- Code: `Arduino/CMRINet-examples/MRCS_cpNode_kernel/MRCS_cpNode_kernel.ino`, 1553 lines, v1.6 (05/25/2021, Catania/Plocher). Also read `Notes.md` (75 ln), `doc.md`, `README.md`.
- Targets: BBLeo (ATMega32u4, CMRI on `Serial1`) and Pro Mini (ATmega328, CMRI on `Serial`). Review is static; nothing was compiled or executed.

## Architecture summary
Single-threaded superloop. `loop()` (`.ino:1522`) calls `CMRI_Read()` (`.ino:1202`) which returns immediately if no byte is buffered, otherwise **blocks** until it has consumed one complete frame (or errors). Frame parsing is a byte-switch (`STX`/`ETX`/`DLE`/`SYN`/default) filling a single shared 260-byte buffer `CMRInet_Buf` used for both RX and TX. Message dispatch: I → `Initialize_cpNode()`, P → `CMRI_Poll_Resp()` (immediate response from pre-latched input bytes), T → `Unpack_Node_Outputs()` + IOX. Non-addressed / errored frames go to `Flush_CMRInet_To_ETX()`. After every dispatch, inputs are re-read (`Pack_Node_Inputs`, `Pack_IOX_Inputs`) with blocking `delay()` calls. No TXEN pin management (assumes hardware auto-direction RS-485). No interrupts used beyond the stock `HardwareSerial` ISR; no watchdog.

## Findings

### [BUG] Blocking byte read with no timeout — one partial frame hangs the node forever
- Spec: LCS-9.10.1 p.8 §D.2 (ln 244-247) puts timeout handling on the Host, but a node that stops responding to all subsequent polls defeats that recovery.
- Code: `Read_CMRI_Byte()` `.ino:1174-1180` — `while (true) { if (available()) return...; }`.
Once `CMRI_Read()` has seen the first byte of anything (even line noise), every subsequent header/body byte is fetched via this infinite busy-wait. If a frame is truncated (host crash mid-message, cable pull, glitch byte with no following traffic), the node spins in `Read_CMRI_Byte()` forever: no polls answered, no inputs scanned, outputs frozen, and no watchdog to reset it. Concrete scenario: a single noise byte arrives while the bus then goes idle → permanent lockup until power cycle.
Fix: add a per-byte timeout (e.g., `millis()` bound ≈ 10 character times) that aborts to `respErr`.

### [BUG] `inCnt` is not reset when STX is found — pre-STX garbage is prepended to the message body
- Spec: p.2 §A (ln 65-67): anything not meeting the full format "is not a valid message"; body starts after MT (p.5 ln 144-155).
- Code: `.ino:1219-1220` (`inCnt = 0` only at function entry); default case `.ino:1303-1310` stuffs bytes into `CMRInet_Buf[inCnt++]` *before* any STX has been seen (`inData` is never checked in the default case); the STX case `.ino:1226` does not reset `inCnt`.
Scenario: one stray byte (noise, or the tail of a foreign message after the early-exit flush, see below) is buffered; the host's next `T` frame follows. The stray byte lands at `CMRInet_Buf[0]`, the real output bytes at `[1..]`. `Unpack_Node_Outputs()` (`.ino:1082-1087`) reads `CMRInet_Buf[0..nOB-1]` → **all outputs are driven from shifted/garbage data** (signals set to wrong aspects) with no error indication. Same corruption applies to I-message parsing (`CMRInet_Buf[0]` expected to be NDP, `.ino:1142`).
Fix: reset `inCnt = 0` in the STX case, and ignore non-protocol bytes while `!inData`.

### [BUG] Buffer overrun guard is off by one (actually two) — out-of-bounds writes at indices 260 and 261
- Spec: p.2 (ln 75) allows bodies up to 256 bytes; the 260-byte buffer is intentionally padded (`.ino:53`, Notes.md v1.4.2).
- Code: `CMRInet_Buf[CMRInet_BufSize]` with `CMRInet_BufSize = 260` (`.ino:254, 264`, valid indices 0-259). Guard: `if (inCnt > CMRInet_BufSize)` `.ino:1315`. With `inCnt == 260` the guard is false, so the next byte writes `CMRInet_Buf[260]` (OOB #1), then `inCnt == 261` trips the guard and `.ino:1327` writes the NUL terminator to `CMRInet_Buf[261]` (OOB #2). Even a *legal* exit with `inCnt == 260` writes the terminator OOB.
Because finding #2 lets pre-STX garbage inflate `inCnt`, this is reachable without a hostile host. On AVR the two clobbered bytes are adjacent globals (link-order dependent — likely `OB[]`/`IB[]`), i.e., silent I/O state corruption.
Fix: guard with `inCnt >= CMRInet_BufSize - 1` before each store.

### [SPEC-DEVIATION] `Flush_CMRInet_To_ETX()` exits when the UART buffer is momentarily empty, not at ETX
- Spec: p.5 §D (ln 157-161): a non-addressed node "discards all the remaining bytes until an ETX is seen"; also p.3 §A (ln 97-100).
- Code: `.ino:1156-1167` — the `else { done = true; }` branch exits the flush the instant `available() == 0`. This was a deliberate v1.4.2 change (Notes.md ln 54, header `.ino:54`) to avoid blocking, but at any baud rate the inter-character gap plus the ~4 ms of `Pack_Node_Inputs()` debounce delays guarantees the flush usually ends mid-frame of the foreign message.
Consequence: the remainder of the foreign frame is re-parsed by the next `CMRI_Read()` as if it were the start of a new message. Usually this ends at the foreign ETX with `respErr` (harmless but per finding #1 it *blocks* until that ETX arrives). The dangerous case is a raw 0x02 in the residue (see next finding) which fabricates a phantom STX. Combined with finding #2, foreign-frame residue is also how garbage gets prepended to genuine bodies.
Fix in a new implementation: make "ignoring" a *state* of the non-blocking parser (discard-until-ETX state), not a separate blocking drain.

### [SPEC-AMBIGUITY] The spec exempts I-message bodies from DLE, so raw STX/ETX/DLE values can legitimately appear on the wire inside someone else's frame
- Spec: p.5-6 §D.a (ln 166-171) mandates DLE insertion only "when forming a Transmit Data or Receive Data message". Yet Table 1 (p.7) defines CT values 2, 3, and 16 (OXXX=2, and `<dH><dL>`/NS may be small integers), and p.5 (ln 157-161) tells non-addressed nodes to scan for ETX.
This is self-contradictory: a spec-compliant host initializing a SUSIC with CT=3 emits a raw 0x03 that every other listening node must treat as ETX — early-terminating their flush — and a raw 0x02 (CT=2) that a lenient parser can mistake for STX. There is no way to write a byte-scanning node that is robust against compliant I traffic for *other* nodes. Any master-library design must either always DLE-escape I bodies (de-facto JMRI-style behavior) or accept that classic nodes will desync/resync on such frames.
Concrete failure in this code: host sends `I` to another node with a CT byte of 2; our node (mid-flush residue, finding #4) sees STX, reads the next two bytes as UA/MT — if the UA byte happens to equal `nodeID+65`, a phantom `T` message can drive outputs from arbitrary data.

### [SPEC-DEVIATION] DLE un-escaping is applied to *all* message bodies, including I (spec: only T/R)
- Spec: p.6 §D.a (ln 166-168).
- Code: the `case DLE:` branch `.ino:1287-1294` runs for every received frame regardless of message type.
Two-sided interop consequence: (a) a host that escapes I bodies (JMRI does) is parsed correctly — good; (b) a strictly spec-compliant host sending a *raw* 0x10 as `<dL>` (transmission delay 16 units) is mis-parsed: the DLE case swallows `<NS>` as the literal delay byte and shifts the rest of the body. Similarly raw 0x03 as `<dL>` (delay 3) terminates the frame early and `Initialize_cpNode()` (`.ino:1126-1128`) reads stale buffer bytes as DLH/DLL. Given ambiguity #5, unescaping everywhere is the more defensible choice, but it should be documented as the de-facto convention: **hosts must DLE-escape I bodies when talking to this node.** This fielded behavior is exactly the kind of de-facto rule a master engine must follow.

### [SPEC-DEVIATION] Parser never requires SYN SYN before STX — bare STX anywhere starts a frame
- Spec: p.2 §A (ln 65-67) "Any sequence of bits not meeting the full specification of this general message format is not... a valid message"; p.5 (ln 144-146) messages *begin* with two SYNs.
- Code: `case SYN:` `.ino:1296-1301` merely ignores SYN outside data (and correctly stores 0xFF *inside* data); the STX case `.ino:1226` fires on any 0x02 regardless of preceding SYNs.
Lenient acceptance improves tolerance of dropped SYNs but is what makes the phantom-STX attack in findings #4/#5 possible. A master library should note: fielded cpNodes accept SYN-less frames, so extra/missing SYNs are not an interop problem, but master-side parsers should *require* at least one SYN or use it to arm the STX detector to reduce desync.

### [BUG] Initialization: DL is set from the buffer *before* NDP is validated, with no length check
- Spec: p.6 §D.1 (ln 192-206): NDP selects node-type-specific handling; `<dH><dL>` follow NDP.
- Code: `.ino:1122-1145` — `DLH = CMRInet_Buf[1]; DLL = CMRInet_Buf[2]; DL = ...` happens unconditionally; only afterwards is `CMRInet_Buf[0] == 'C'` checked, and then only to call the empty `Process_cpNode_Options()`. There is no check that the body actually contained ≥3 bytes (`inCnt` is not consulted; a truncated `I` frame reads stale bytes left over from previous traffic, since the buffer is shared with TX and only NUL-terminated at `inCnt`).
Scenario: host broadcast-misaddresses an SMINI-style `I` (NDP='M') to this node, or an I frame is truncated → the node silently adopts a bogus per-character transmit delay of up to 655 ms/char, making its poll responses appear as timeouts to the host. Fix: validate `inCnt >= 3` and NDP == 'C' before applying anything.

### [BUG] `delayMicroseconds(DL)` overflows for DL > 16383 µs
- Spec: p.6 (ln 203-206) allows dH/dL up to 0xFFFF units × 10 µs = 655,350 µs.
- Code: `.ino:1419-1421`. AVR `delayMicroseconds()` takes `unsigned int` and is documented accurate only to 16383 µs; `DL` is `unsigned long` (`.ino:269`). Values above 16383 truncate/wrap to an essentially random shorter delay.
Low practical impact (modern hosts send 0), but a master engine must know real nodes do not faithfully honor large delays.

### [SPEC-AMBIGUITY] Is the transmission delay per character or per message?
- Spec: p.6 (ln 203-206): "A non-zero transmission delay value will cause ... Nodes to delay **between transmissions** to the Host" — it never says per character or per message.
- Code: applies it **per transmitted character** (`.ino:1414-1422`, inside the byte loop), matching Chubb-era intent (pacing slow hosts). Note the delay is inserted after `write()` into the interrupt-driven 64-byte TX buffer, so wire-level inter-character gaps only materialize once DL exceeds the character time. A master engine should treat dH/dL as "per character, approximately".

### [BUG] No length validation on T messages — short frames drive outputs from stale buffer bytes
- Spec: p.8 §D.4 (ln 266-269): NO is "the total number of output card bytes"; spec is silent on short frames.
- Code: `Unpack_Node_Outputs()` `.ino:1082-1087` copies `CMRInet_Buf[0..nOB-1]` and `Unpack_IOX_Outputs()` `.ino:443-448` copies `CMRInet_Buf[nOB..nOB+numIOX_OUT-1]` without checking how many bytes were actually received (`inCnt` isn't passed or checked; buffer is shared with the TX path so stale content includes the node's own last poll response).
Scenario: host sends `T` with 2 bytes to a node configured with 16 IOX output bytes → 16 IOX ports get written from leftovers of the node's previous R response. Silent, non-deterministic output corruption. Fix: track body length and zero-fill or ignore missing bytes.

### [BUG] IOX I2C read failure latches phantom inputs (Wire.read() == -1)
- Spec: n/a (hardware robustness).
- Code: `.ino:488-489` — `Wire.requestFrom(...)` return value ignored; on NACK/absent chip `Wire.read()` returns -1, and `IOX_inBuf[i] |= -1` sets **all 16 bits** of the `int` (`.ino:319`). The poll response then reports 0xFF for that byte until cleared.
Scenario: transient I2C glitch or unpowered IOX card → host sees every occupancy detector on that byte occupied. Fix: check `requestFrom()` count before OR-ing.

### [DESIGN-LIMITATION] No RS-485 TXEN/driver-enable discipline and no `flush()` before "TX done"
- Spec: p.4 §C (ln 117-120): four-wire half-duplex RS-422/485; all node transmitters share the host-receive pair, so drivers must tri-state when idle.
- Code: `CMRI_Poll_Resp()` `.ino:1412-1422` just `write()`s; there is no DE/RE pin, no `CMRI_SERIAL.flush()`, no post-drain turn-off. This only works because MRCS cpNode hardware provides automatic transmit-enable in the transceiver circuit. Any port of this code to hardware with a GPIO-controlled DE pin would need assert-before-first-byte and drop-after-`flush()` logic that simply does not exist here. A master library must not assume nodes yield the pair promptly under all hardware variants.

### [DESIGN-LIMITATION] Blocking debounce/scan delays risk UART RX overflow at high baud
- Spec: p.4 §C (ln 122-123) requires support up to 115200 BPS.
- Code: `Pack_Node_Inputs()` runs 2 × `delay(2)` every loop (`.ino:1049-1066`), plus `Pack_IOX_Inputs()` adds `delay(2)` **per IOX input port** (`.ino:493`) plus I2C transaction time. With 8 IOX input ports that is ≥ 20 ms per loop with interrupts servicing a 64-byte `HardwareSerial` RX buffer. At 115200 BPS (~11.5 bytes/ms) any frame longer than ~64 bytes addressed to *any* node on the wire during that window overflows the buffer and drops bytes — likely eating an ETX and triggering the finding-#1 lockup or finding-#2 corruption. At the historically common 9600-28800 BPS this is a non-issue, which is probably why it survived in the field.

### [DESIGN-LIMITATION] Signed-char protocol constants — works on AVR, breaks on ARM/ESP32
- Code: `const char SYN = 0xFF` (`.ino:227`) is -1 on AVR (char signed); `Read_CMRI_Byte()` returns `char`, `c` is `int` (`.ino:251`), so incoming 0xFF sign-extends to -1 and `case SYN:` matches. On targets where `char` is unsigned by default (ARM GCC, hence ESP32/Teensy), `SYN` becomes 255 while... the read path also changes, and `matchID - UA_Offset` debug math, `switch(int(c))` case values, and the `> 127` clamp in `SetNodeAddr` (`.ino:518-528`) all silently shift meaning. Any reuse of this parser must normalize to `uint8_t` immediately at the read.

### [DESIGN-LIMITATION] Onboard inputs are not latched (comment says they are); IOX inputs are — inconsistent input semantics
- Code: comment `.ino:476` ("bits will be latched and cleared when sent in the poll response") and `IB[j] = 0` in `CMRI_Poll_Resp()` (`.ino:1384`) imply latching, but every `pack_*` routine zeroes IB first (`.ino:560, 621, 671, ...`), and `Pack_Node_Inputs()` runs every loop — so `IB[j] = 0` is dead code and a pulse shorter than one poll interval on an onboard pin is lost. IOX inputs genuinely OR-latch (`.ino:489`) and clear on poll (`.ino:1403`), correctly capturing short pulses. The v1.6 author's own comment (`.ino:1041-1044`) also documents that the "debounce" is read-twice-discard-first, not a compare. A master engine should not assume nodes latch inputs between polls.

### [DESIGN-LIMITATION] Debug builds hang at boot without a USB host attached
- Code: `.ino:1457-1459` — `while(!MONITOR_SERIAL) {}` blocks until the Leonardo's CDC port enumerates. A node flashed with any DEBUG_* flag true and deployed on the layout (no USB) never starts. Minor, but a classic field footgun.

## Spec ambiguities encountered
1. **I-message bodies vs DLE and flush-to-ETX** (p.6 §D.a ln 166-168 vs p.7 Table 1 vs p.5 ln 157-161): CT/dH/dL values of 2/3/16 are legal raw bytes in I bodies, which breaks both ETX-scanning by non-addressed nodes and any STX-anywhere parser. The spec is self-contradictory here; de-facto practice (this node, JMRI) is to DLE-process every body.
2. **dH/dL semantics** (p.6 ln 203-206): "delay between transmissions" — per character or per message is unspecified; de-facto (this code) is per character.
3. **0xFF (SYN) inside message bodies** (p.5-6 §D.a): the spec does not require escaping 0xFF and never states how a receiver should treat 0xFF mid-body. This code does the sensible thing (stores it when in data, `.ino:1296-1297`, and deliberately does not escape it on TX, `.ino:1376`), but the spec should say so explicitly.
4. **Pre-initialization behavior** (p.6 ln 195: "Each Node ... must have an initialization message sent"): the spec never says whether a node may answer polls before receiving I. This node answers immediately after boot with compile-time configuration; a master must tolerate R responses from never-initialized nodes.
5. **Short/oversized bodies**: spec bounds the body at 0-256 bytes (p.2 ln 75) but defines no receiver behavior for truncated, oversized, or unknown-MT frames. Every implementation invents its own (this one: flush-to-ETX + silent stale-data reuse).

## Strengths
- **[STRENGTH] TX-side DLE escaping is exactly per spec** (p.6 §D.a): escapes STX/ETX/DLE and deliberately not SYN, with an explanatory comment (`.ino:1374-1383`) — the clearest statement anywhere of the de-facto "don't escape 0xFF" convention.
- **[STRENGTH] DLE is honored before the ETX/STX cases on receive** — because `case DLE:` performs its own immediate read of the escaped byte (`.ino:1287-1288`), an escaped 0x03/0x02/0x10 can never falsely terminate or restart a frame, including in the last body position.
- **[STRENGTH] Poll turnaround is minimal**: the R response is assembled from input bytes latched during the previous loop pass and transmitted immediately on P (`.ino:1538`, `.ino:1551-1552`) — no input scanning happens between poll receipt and response, which matters on a polled half-duplex bus.
- **[STRENGTH] Correct UA offset handling and address clamp** (spec p.5 ln 148-151): `UA = nodeAddr + 65` stored once (`.ino:526`), compared directly against the wire byte; out-of-range configured addresses are clamped (`.ino:522-524`).
- **[STRENGTH] Buffer sized for the worst-case network frame** (260 = max SUSIC 256 + pad, `.ino:53, 254`) with an overrun check that degrades to `respErr` + flush rather than continuing to parse — the *intent* is right even though the bound is off by one (finding #3).
- **[STRENGTH] 8N1 framing matches the spec** (p.2 §A ln 55): `CMRI_SERIAL.begin(CMRINET_SPEED)` defaults to SERIAL_8N1 (`.ino:1467`). Useful de-facto data point: fielded cpNodes are 8N1; if any host stack (e.g., older JMRI SUSIC configs) transmits 8N2, an 8N1 receiver tolerates the extra stop bit as idle, so 8N1-on-node is the interop-safe choice.
- **[STRENGTH] IOX input OR-latching between polls** (`.ino:489`, cleared at `.ino:1403`) captures short pulses that a naive read-at-poll design would miss.

## Master-library implications
What this fielded node teaches a CMRInet HOST/master engine:
- **Always DLE-escape every message body you transmit, including I messages.** This node unescapes everything; a raw 2/3/16 in an I body mis-parses on it (and per ambiguity #1 the spec cannot be followed literally anyway). This is the single most important de-facto rule.
- **Never rely on nodes to be resilient**: this node can be hung forever by a truncated frame (finding #1) and can drive corrupted outputs from desync (findings #2, #4). The master should (a) always emit clean, complete frames — both SYNs, no mid-frame pauses long enough to let flush-on-empty fire; (b) space consecutive frames so a node's flush/parse residue from message N can't bleed into message N+1; (c) treat a persistently silent node as needing operator attention, not just retries.
- **Expect immediate poll responses** from cpNode-class hardware (sub-millisecond turnaround at the node; latency is dominated by line time). Master timeout can be tight — a few character times plus one loop worst case (~25 ms with heavy IOX config) — but see the per-character DL caveat.
- **Treat dH/dL as per-character delay, capped in practice at ~16 ms/char on AVR nodes** (findings #8/#9). For modern masters just send 0.
- **Do not send T bodies shorter than the node's configured width** — this node fills the gap with stale RAM (finding #10). Always send the full output image.
- **Reuse**: the DLE-before-ETX receive ordering, the TX escaping table with its SYN comment, the pre-latched poll-response pattern, and the 260-byte worst-case buffer sizing are all worth carrying forward.
- **Avoid**: shared RX/TX buffer (stale-data hazards, findings #7/#10), blocking reads without timeouts, flush-until-empty as a substitute for a discard-until-ETX parser state, `char`-typed protocol constants, and `inCnt`-style counters that aren't reset at frame start. A master engine should be a strict non-blocking state machine: IDLE → (SYN*) STX → UA → MT → BODY(DLE-aware, length-bounded, reset at STX) → ETX, with a discard-to-ETX state and per-byte timeout.
