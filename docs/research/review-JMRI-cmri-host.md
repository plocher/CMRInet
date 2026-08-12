# JMRI — CMRInet Host-side review

## Scope & sources

Adversarial review of JMRI's C/MRI serial stack acting as the CMRInet **Host**
(master), against NMRA LCS-9.10.1 CMRInet Protocol v1.1
(`docs/lcs-9.10.1_cmrinet_v1.1.pdf`, 9 pp.). JMRI is the dominant fielded Host;
its behavior is the de-facto answer key every modern Node is tuned against.

Code reviewed (local clone at `/Users/jplocher/Dropbox/Arduino/JMRI`, paths
below relative to `java/src/` unless noted; reviewed read-only, current
checkout):

- `jmri/jmrix/cmri/serial/SerialTrafficController.java` (STC) — poll engine, framing
- `jmri/jmrix/cmri/serial/SerialMessage.java`, `SerialReply.java` — payload objects
- `jmri/jmrix/cmri/serial/SerialNode.java` — node model, I/T packet construction, timeout policy
- `jmri/jmrix/cmri/serial/serialdriver/SerialDriverAdapter.java` — port config
- `jmri/jmrix/AbstractMRTrafficController.java` (AMRTC), `AbstractMRNodeTrafficController.java`, `AbstractMRMessage.java`, `AbstractMRReply.java` — shared engine the CMRI stack rides on
- `jmri/jmrix/cmri/serial/{sim,networkdriver,diagnostic,serialmon,cmrinetmetrics,cmrinetmanager,packetgen,nodeconfigmanager}/…` — simulator, TCP transport, diagnostics
- Tests: `java/test/jmri/jmrix/cmri/serial/SerialNodeTest.java` and siblings (used as evidence of intended behavior)

Context built upon (not repeated): `docs/research/comparison.md` §1 (spec
defects) and §2 (de-facto wire conventions from the four node reviews).

## Architecture summary

JMRI's CMRI Host is two threads over a serial (or TCP) byte stream:

- A **transmit thread** (`AMRTC.transmitLoop()`, AbstractMRTrafficController.java:360-521)
  that alternates between draining an explicit message queue (user tools) and,
  when idle, calling `pollMessage()` (SerialTrafficController.java:179-249) to
  drive a round-robin **poll engine** over the registered `SerialNode` list.
  Each engine step services exactly one node with exactly one message, chosen
  by priority: **I** (if `mustInit`), else **T** (if `mustSend`, i.e. outputs
  changed), else **P** (if the node has registered input sensors and per-node
  polling enabled). After every send it blocks in `transmitWait()` for the
  message's timeout or until the receive thread signals a completed reply.
- A **receive thread** (`AMRTC.receiveLoop()` → `handleOneIncomingReply()`,
  AbstractMRTrafficController.java:910-1211) using CMRI-specific
  `waitForStartOfReply()` (hunt for STX) and `loadChars()` (read to ETX with
  DLE-unescape) overrides (SerialTrafficController.java:341-360). Completed
  replies are dispatched on the Swing EDT to all listeners (sensor manager,
  monitor, metrics collector).

Message payloads (`SerialMessage`) carry **UA and MT plus already-DLE-stuffed
body**; the framing layer adds only SYN SYN STX in front and ETX behind
(SerialTrafficController.java:369-401). DLE stuffing therefore happens at
packet-construction time inside `SerialNode.createInitPacket()` /
`createOutPacket()` (SerialNode.java:928-1199).

## Host wire-behavior facts

The authoritative [HOST-FACT] list for CMRInet. Every fact verified in
code; citations given.

**Framing (TX)**
- Exactly **two SYN (0xFF)**, then STX, UA, MT, body, ETX. No padding, no
  inter-frame filler; the whole frame is built in one buffer and written with a
  **single `write()` + `flush()`** (gapless emission).
  SerialTrafficController.java:369-401; AbstractMRTrafficController.java:693-723.
  Spec: p.3 Fig.1, p.5 lines 144-146.
- **UA = node address + 65**; addresses 0–127 accepted
  (SerialNode.java:637-639, 1148, 1184; SerialMessage.java:86). Spec p.5:148-151.

**DLE escaping (TX)**
- **JMRI DLE-escapes I-message bodies** — yes, definitively. Bytes 2, 3, 16 in
  the body get a DLE prefix, for **both I and T** messages
  (SerialNode.java:1138-1161 for I; 1170-1197 for T). The unit test
  documents it as intended: an X-type init with NS=2 asserts `DLE` at element 5
  before the NS byte (java/test/…/SerialNodeTest.java:173-174). This exceeds the
  spec, which mandates DLE only for T/R (p.6 §D.a:166-171), and **is** the
  de-facto standard comparison.md §1.1 predicted.
- **0xFF is never escaped**, in any message type (no code path escapes it).
- The **header (UA, MT) is never escaped** — correct: UA ≥ 65, MT is a letter.
- P messages have no body; nothing to escape (SerialMessage.java:84-90).
- **Raw 0xFF appears inside CPNODE/CPMEGA I bodies**: 6 reserved pad bytes of
  0xFF terminate the body (SerialNode.java:1082-1086, 1123-1127). Any node that
  resynchronizes on SYN mid-frame will break on a JMRI cpNode init.

**Serial port config**
- **8 data bits, NO parity, 2 stop bits (8N2)**, flow control NONE, RTS & DTR
  asserted: `activatePort(portName, log, 2)` where the third argument is stop
  bits (serialdriver/SerialDriverAdapter.java:26,37-38;
  AbstractSerialPortController.java:117-122). Spec says 10-bit frame = 1 stop
  bit (p.2:54-55) — JMRI deviates, and its 8N2 is the classic-Chubb-compatible
  de-facto TX standard (comparison.md §1.2).
- Baud: 9600/19200/28800/57600/115200, **default 19200**
  (SerialDriverAdapter.java:92, defaultBaudIndex()=1 at 95-97). Matches spec p.4:122-123.
- No RS-485 turnaround/TXEN handling anywhere — JMRI assumes the 4-wire
  RS-422/485 "dongle" topology of spec p.4 §C; the converter handles the bus.
- A raw-bytes-over-**TCP** transport exists (networkdriver/NetworkDriverAdapter.java),
  same protocol engine; timing-slop log messages are demoted for it
  (SerialTrafficController.java:275-281).

**Timeouts, retry, re-init**
- **Poll (P) timeout: 250 ms**, hard constant, set on every poll message
  (`POLL_TIMEOUT`, SerialMessage.java:16, applied at :88).
- **After an I message the engine waits the full 500 ms** timeout — nodes send
  no reply to I, so this is a deliberate 500 ms per-init settle/stall
  (SerialTrafficController.java:203; the engine always expects a reply,
  AbstractMRMessage.java:120-122).
- **After a T message the engine waits 2 ms** (same mechanism,
  SerialTrafficController.java:215) — a built-in 2 ms post-T gap.
- Messages sent by user tools default to 2000 ms (`SHORT_TIMEOUT`,
  AbstractMRMessage.java:154, ctor :24).
- **No retransmission ever** (retries default 0 and are only used for
  port-not-ready, AbstractMRMessage.java:179; AbstractMRTrafficController.java:720-740).
  On timeout: log, count, move to next node — exactly spec p.8:243-247.
- **Re-init policy: after MORE THAN 5 consecutive P timeouts** (i.e. on the
  6th), the node is scheduled for re-initialization: counter reset to 1,
  `mustSend` set (forces a full T after the I), all its sensors forced to
  UNKNOWN, and `mustInit` set (SerialNode.handleTimeout, SerialNode.java:1301-1336;
  SerialTrafficController.handleTimeout, :287-301). A silent node is retried
  forever — never marked dead; each revolution it costs one 250 ms timeout,
  and every 6th miss adds an I (+500 ms) and T. Timeouts to I and T messages
  do not trigger this logic (only MT=='P' counts, SerialNode.java:1304), and a
  successful reply zeroes the counter (resetTimeout, SerialNode.java:1339-1344).

**Poll engine / scheduling**
- Poll order = **node registration order** (`nodeArray`), round-robin via
  `curSerialNodeIndex` (SerialTrafficController.java:193-197, 254-259;
  AbstractMRNodeTrafficController.java:99-121). The `pollListPosition` /
  `cmriNetPollList` seen in the CMRInet manager UI is bookkeeping only, not
  consulted by the engine (SerialTrafficController.java:141-146;
  cmrinetmanager/CMRInetManagerFrame.java:237).
- **One message per engine cycle**; between idle cycles the transmit thread
  sleeps **5 ms** (`mWaitBeforePoll = 5`, SerialTrafficController.java:46;
  wait logic AbstractMRTrafficController.java:447-460). Reply arrival wakes it
  immediately, so a healthy node costs ≈ 5 ms + its reply latency.
- **T is sent only on change** (`mustSend` set when an output byte actually
  changes, SerialNode.java:233-235), and **always carries the full output
  image**: `numOutputCards() × bitsPerCard/8` bytes from a clone of
  `outputArray` (SerialNode.java:1169-1199). Never a partial frame.
- **I precedes the first P for every node** (`mustInit` starts true for all,
  AbstractMRNodeTrafficController.java:29-40, 103) and **a full T immediately
  follows every I** because the constructor and the re-init path both set
  `mustSend` (SerialNode.java:167, 1318). After sending a T the poll pointer
  is rewound so the same node is serviced next cycle
  (SerialTrafficController.java:219) — per-node order on the wire is I → T → P.
- **Only nodes with ≥ 1 registered input sensor are ever polled**
  (`getSensorsActive()`, SerialTrafficController.java:234-247). Output-only
  nodes receive I and T but never P. Per-node polling can also be disabled
  (AUTOPOLL option bit, SerialNode.java:600-608); a global kill switch exists
  (`setPollNetwork`, SerialTrafficController.java:139-154).

**Receive path (R parsing)**
- Frame start = **hunt for a bare 0x02**; SYNs are skipped as noise and NOT
  required (SerialTrafficController.java:356-360). The Host is
  preamble-tolerant; it would accept a frame with zero or five SYNs.
- Body is read byte-at-a-time until an **unescaped 0x03**; on DLE the next
  byte is taken as data unconditionally. ETX is tested before DLE handling, so
  `DLE 03` correctly stores 0x03 (SerialTrafficController.java:341-353).
- Element 0 = UA, element 1 = MT, data from element 2 — but the framing layer
  performs **no UA, MT, or length validation whatsoever** (see Findings 5, 6).
- Max reply size = **120 bytes including UA+MT** (`AbstractMRReply.DEFAULTMAXSIZE`,
  AbstractMRReply.java:210-213, not overridden by `SerialReply`) → max 118
  input-data bytes (see Finding 4).
- Raw 0xFF within an R body is stored as data (correct; nothing special-cases it).
- No inter-byte timeout at protocol level; `readByteProtected` loops on the
  (possibly semi-blocking) stream forever (AbstractMRTrafficController.java:989-1011).
  Loss of the trailing ETX parks the parser mid-frame (see Finding 7).

**I-message contents (dH/dL, NS/CT, dialects)**
- **dH/dL = transmissionDelay/256, remainder** (units of 10 µs per spec p.6);
  JMRI default 0, configurable 0–65535 via node config UI
  (SerialNode.java:74, 150, 657-668, 963-970; test evidence: 2000 → 0x07,0xD0,
  SerialNodeTest.java:159, 171-172). Sent for ALL node types including CPNODE.
- **SMINI ('M'=77)**: NS = number of 2-lead searchlight (yellow-oscillate)
  pairs; if NS > 0 exactly **6 CT bytes** follow — a 48-bit LSB-first mask over
  the 6 output ports marking both bits of each pair; **if NS == 0 no CT bytes**
  (SerialNode.java:1011-1029; tests SerialNodeTest.java:105-152 incl. the
  User-Manual B10 example). Spec p.7:212-215, 228-231.
- **USIC/SUSIC ('N'=78 / 'X'=88)**: NS = ceil(totalCards/4); each CT byte
  packs 4 card slots base-4, I=1, O=2, X=0, weight 1/4/16/64
  (SerialNode.java:1033-1046) — reproduces spec Table 1 exactly
  (e.g. IOOO = 1+2·4+2·16+2·64 = 169). Bits-per-card 24 vs 32 selects N vs X
  (SerialNode.java:950-952).
- **CPNODE ('C'=67) I-body dialect** — the exact bytes a cpNode must parse:
  `<'C'> <dH> <dL> <opts1> <opts2> <NI> <NO> <0xFF ×6>` — body length 13
  (SerialNode.java:1049-1088; `INITMSGLEN=12` + NDP). opts1 bit0=USECMRIX,
  bit1=SENDEOT, bit2=USEBCC (SerialNode.java:111-115), all default 0; opts2
  reserved. NI/NO are the counts of 8-bit input/output "cards" including the
  2+2 onboard ports (cardTypeLocation[0..3] fixed I,I,O,O, SerialNode.java:432-445;
  IOX cards assigned from slot 4, nodeconfigmanager/NodeConfigManagerFrame.java:1796-1827).
  All bytes after NDP are DLE-escaped when equal to 2/3/16 (so NI=2, opts=16
  etc. go out escaped); the 0xFF pads are not.
- **CPMEGA ('O'=79)**: same body layout as CPNODE, 8 onboard bytes
  (SerialNode.java:65, 447-464, 1090-1129). **'O' is not in LCS-9.10.1 at all.**

## Findings

### 1. [HOST-FACT] JMRI escapes I-message bodies — the I-parseability question is settled
SerialNode.java:1138-1161; SerialNodeTest.java:173-174; spec p.6 §D.a:166-171.
The spec's worst defect (comparison.md §1.1 — I bodies not parseable under
spec-literal rules) is resolved by the dominant Host exactly as the node
reviews predicted: DLE-stuff 2/3/16 in every body including I, never 0xFF.
Any node that does NOT unescape I bodies mis-parses JMRI inits whose dH/dL,
NS, CT, opts, NI or NO bytes are 2/3/16; any Host that doesn't escape them
breaks fielded nodes. CMRInet must do exactly what JMRI does.

### 2. [SPEC-DEVIATION] 8N2 serial framing, not the spec's 8N1
serialdriver/SerialDriverAdapter.java:26; AbstractSerialPortController.java:117-122;
spec p.2:54-55. JMRI transmits and receives at 8N2 with no parity. Since a
2-stop-bit TX is readable by an 8N1 RX, this is benign for nodes but it IS the
number fielded nodes were tuned against — and confirms comparison.md §1.2's
recommendation (8N2 TX, stop-bit-tolerant RX) as the interop-safe choice.

### 3. [SPEC-DEVIATION] Protocol extensions: CPMEGA NDP 'O' and CMRI-E message types
SerialNode.java:59, 65, 1090-1129; SerialReply.java:52-60;
cmrinetmetrics/CMRInetMetricsCollector.java:58-85. JMRI defines NDP 'O'
(CPMEGA) and recognizes reply/message types 'E' (EOT), 'Q', 'D', 'W', 'A',
'C', 'M' — none defined in LCS-9.10.1 (p.5:153-155 lists only I/P/R/T). A
bench Host should at minimum not choke on these MTs, and should decode 'E'
(cpNode SENDEOT option emits it) and NDP 'O'.

### 4. [BUG] Replies hard-capped at 120 bytes — long R messages desync the receiver
SerialTrafficController.java:341-353 (loop bound `msg.maxSize()`);
AbstractMRReply.java:210-213 (`DEFAULTMAXSIZE = 120`, not overridden);
spec p.2:75 (body 0–256 bytes). An R message with more than 118 data bytes
(legal: NI up to 256; a fully loaded SUSIC easily exceeds this) fills the
120-element buffer and `loadChars` returns **without consuming through ETX**.
The residual bytes are then hunted for 0x02: any data byte 0x02 starts a
phantom frame whose "UA/MT" are input data — feeding garbage to whatever node
that fake UA maps to (via Finding 6), and at best logging unexpected-state
errors until resync at the real ETX. Consequence for CMRInet: size the
RX buffer for ≥ 256 data bytes + header, and treat "JMRI can't poll nodes with
>118 input bytes" as a fielded-ecosystem ceiling worth documenting.

### 5. [BUG] No UA/MT verification in the transmit-wait path
AbstractMRTrafficController.java:1095-1150; SerialTrafficController.java (no
override adds validation). ANY STX…ETX sequence — from the wrong node, of the
wrong type, or a phantom frame per Finding 4 — releases `WAITMSGREPLYSTATE`
and is treated as "the" reply to the outstanding poll, ending the 250 ms wait
early and advancing the engine. A slow node answering after its window closes
is then processed while a different node's poll is outstanding (logged only as
`unexpectedReplyStateError`, SerialTrafficController.java:270-284). The engine
never cross-checks that the R's UA matches the P it sent.

### 6. [BUG] Sensor data routed by UA with no MT check — any reply body becomes input bits
SerialSensorManager.java:122-129 (`reply()` calls `node.markChanges(r)` with
no `isRcv()` test); SerialNode.markChanges:1215-1261 reads elements 2+ of
whatever it is given. Consequence: on any link where the Host hears non-R
traffic — a half-duplex/echoing adapter reflecting the Host's own T frames, or
a node emitting extended types with a body — the body bytes are decoded as
that UA's sensor states. An echoed `T` to node 3 flips node 3's sensors.
(An echoed short frame like P or EOT is harmless: `loc+2 >= numElements`
skips.) The DiagnosticFrame gets this right (`waitingOnInput && l.isRcv() &&
testNodeAddr == l.getUA()`, diagnostic/DiagnosticFrame.java:1306) — the sensor
manager does not.

### 7. [BUG] Dangling DLE eats ETX; no protocol-level inter-byte timeout
SerialTrafficController.java:348-350. A malformed reply ending `… DLE ETX`
(node bug, or a byte lost to line noise leaving DLE last) stores the real ETX
as data and keeps reading into the next frame — the two frames merge, the
merged reply is garbage, and per Finding 6 garbage becomes sensor data. There
is no inter-byte timeout that abandons a stuck partial frame: a reply
truncated before ETX leaves the receive thread parked in `loadChars`
indefinitely while the transmit engine (after its 250 ms timeout) keeps
transmitting; the node's NEXT reply is then appended to the stale half-frame.
Recovery only happens at the next 0x03 on the wire. The node reviews flagged
this same defect class in node code (comparison.md §3); JMRI shows the Host
side needs the inter-byte timeout just as much.

### 8. [BUG] Timeout accounting attributed to the wrong node (poll-pointer race)
SerialTrafficController.java:219 (pointer rewound when a T is dispatched),
287-301 (`handleTimeout` resolves the node via `curSerialNodeIndex`);
SerialNode.java:1302 (`timeout++` unconditionally, before the MT check).
Every T message "times out" by design (2 ms wait, no reply), and at that
moment `curSerialNodeIndex` has been rewound to the PREVIOUS node — so the
previous node's consecutive-timeout counter is incremented for a message sent
to a different node. Effects: `isPollingOK()` (SerialNode.java:1292-1294) and
the TIMEOUT poll status/metrics can indict healthy nodes, and a node one miss
away from re-init can be pushed over the threshold by traffic to its neighbor
(the MT=='P' guard stops false re-inits from the T itself, but the counter
pollution is real). Same aliasing applies to `resetTimeout` (:303-310).

### 9. [DESIGN-LIMITATION] One silent node degrades the whole bus by 250 ms per revolution
Single transmit thread, serialized `transmitWait` per message
(AbstractMRTrafficController.java:419-437); 250 ms constant
(SerialMessage.java:16). With one dead node, every poll revolution stalls
250 ms (plus 500 ms every 6th revolution for the re-init I); N dead nodes
stack. Output (T) latency for healthy nodes rides the same loop, so a dead
node visibly slows turnout response layout-wide. The per-node knobs exist but
are dead code: `initTimeout`/`xmitTimeout` setters are never consulted — the
calls that would use them are commented out
(SerialTrafficController.java:143-144, 204, 216). Poll timeout is not
per-node configurable anywhere. comparison.md §2's "per-node configurable
timeout" is thus a CMRInet improvement, not a JMRI behavior to copy.

### 10. [DESIGN-LIMITATION] Output-only nodes are never polled, never health-checked, never re-inited
SerialTrafficController.java:234-247 (P only if `getSensorsActive()`);
SerialNode.java:1301-1336 (re-init driven solely by P timeouts). A node with
no registered input sensors gets I once and T on changes, forever — if it
power-cycles, nothing re-initializes it (its outputs stay dark until JMRI
restarts or a user forces an init). Fielded practice consequence: cpNode
users define a dummy input to keep nodes polled. A bench Host should poll
every node regardless, or offer a keepalive/health P.

### 11. [DESIGN-LIMITATION] Fixed 500 ms bus stall per I message
SerialTrafficController.java:203; AbstractMRMessage.java:120-122 (reply always
expected). Since no reply to I exists, the full 500 ms is always consumed.
This doubles as the de-facto "node init settle time" that masks the
INIT-swallows-next-frame bugs found in node firmware (comparison.md §2 "after
sending I, immediately follow with a full T" — JMRI does send the T next, but
only after this half-second). A smarter Host wants `replyExpected() == false`
semantics with a configurable settle delay.

### 12. [DESIGN-LIMITATION] Reply dispatch synchronous on the Swing EDT
AbstractMRTrafficController.java:118 (`synchronizeRx = true` default),
1071-1086 (`invokeAndWait`), 562-587 (transmit thread blocks in
`checkReplyInDispatch` until dispatch completes). A busy/hung GUI backpressures
the receive path and delays the next poll. Relevant when using JMRI as a
latency reference: measured poll intervals include EDT scheduling noise
(the metrics tool averages 10 polls partly for this reason,
CMRInetMetricsData.java:168-185).

### 13. [BUG] (minor, latent) DLE-count loop skips byte 0, insert loop does not
SerialNode.java:1140 (`for (int i = 1; …)`) vs :1152 (`for (int i = 0; …)`).
The message is sized counting escapable bytes from index 1 (skipping NDP) but
DLEs are inserted scanning from index 0. Today all NDP values are letters
(77/78/88/67/79) so counts always match; if an NDP were ever a protocol
character the buffer math under-allocates and `setElement` writes past the
declared length. Harmless now, a trap for anyone porting this code.

### 14. [BUG] (minor) `setOutputBit` range guard off-by-one and silent clamp
SerialNode.java:218 (`>` where `>=` is meant — the first byte past the
configured geometry does not warn) and :221-223 (byteNumber ≥ 256 silently
clamped to 255, so an out-of-range bit write corrupts output byte 255 instead
of failing). Cosmetic in practice since `createOutPacket` only transmits the
configured geometry, but the warn-and-continue pattern hides config errors.

### 15. [BUG] (tool-level) Diagnostic frame index errors
diagnostic/DiagnosticFrame.java:1177 (`endInByte = begInByte + portsPerCard`,
missing `-1`, so `reply()` at :1308-1311 copies one byte too many) and :1016
(wraparound invert uses `inBytes[j]` where `inBytes[i]` is meant — with a
nonzero `begInByte` the wrong byte is inverted, producing false compare
errors). Also silently raises observation delay to a 250 ms floor (:840-842).
Worth knowing before trusting the loopback test as a conformance oracle.

### 16. [STRENGTH] Escaping done at packet construction; framing layer is trivial and gapless
SerialMessage carries the pre-stuffed body; `addHeaderToOutput`/
`addTrailerToOutput`/single `write()` mean a frame can never be emitted with
internal pauses or interleaved bytes (SerialTrafficController.java:369-401;
AbstractMRTrafficController.java:693-723). This satisfies the node-side
requirement (comparison.md §2, "emit clean, gapless, complete frames") by
construction. The monitor shows DLEs because they're really in the payload —
honest tracing.

### 17. [STRENGTH] Poll status + metrics as passive bus listeners
Per-node poll status machine (ERROR/IDLE/POLLING/TIMEOUT/INIT,
SerialNode.java:96-101, driven from the engine at
SerialTrafficController.java:205, 231, 236, 295) surfaced in the CMRInet
manager table (cmrinetmanager/CMRInetManagerFrame.java:369, 409); a metrics
collector registered as an ordinary listener counts timeouts,
truncated/unrecognized frames, init messages, and computes a 10-poll moving
average of poll interval (SerialTrafficController.java:48-49;
cmrinetmetrics/CMRInetMetricsCollector.java, CMRInetMetricsData.java:158-185).
Zero coupling to the engine — the exact pattern for CMRInet's stats.

### 18. [STRENGTH] Packet monitor with per-node / per-packet-type filtering and NDP-aware I decoding
serialmon/SerialMonFrame.java:126-238 (message side; decodes I bodies
differently per NDP, prints DL, NS/CT vs opts), 243-300 (reply side, R and
EOT), with per-node enable and per-packet-type filter bits stored on the node
(SerialNode.java:124-125, 614-630) and a dedicated filter UI
(SerialFilterFrame). A raw-vs-decoded toggle handles DLE display (:159-163).
This is the feature checklist for a bench instrument's trace view.

### 19. [STRENGTH] Deliberate re-init hygiene on recovered nodes
SerialNode.java:1313-1332: on the 6th consecutive poll miss, JMRI both forces
re-INIT **and** a full T (`setMustSend`) **and** invalidates all sensor state
to UNKNOWN (with UI callbacks). A recovered node therefore gets I → full
output image → fresh input baseline, and stale sensor reads can't survive an
outage. CMRInet's Host should replicate all three legs.

### 20. [DESIGN-LIMITATION] The "simulator" is a null modem, not a node emulator
sim/SimDriverAdapter.java:66-81 (output discarded, input pipe never written)
with `handleTimeout` stubbed to nothing (:53). It exists so JMRI can run
without hardware; it synthesizes no R replies and models no node behavior.
There is no CMRInet node emulator anywhere in JMRI — the CMRInet
bench-instrument/emulator idea fills a genuine gap.

### 21. [SPEC-AMBIGUITY] Reply expectations for I and T are invented, not specified
Spec defines a timeout only for P (p.8:243-247) and never says whether I or T
may be ACKed. JMRI resolves this as "wait 500 ms / 2 ms then treat timeout as
normal" (SerialTrafficController.java:203, 215, 287-301 comment "timeout to
init, transmit message is normal"). Meanwhile the cpNode SENDEOT option makes
nodes send an 'E' frame after T — which JMRI's engine happily accepts as the
"reply" that ends the 2 ms wait (Finding 5's looseness doubling as the
mechanism that makes EOT work). A spec revision should define this.

### 22. [SPEC-AMBIGUITY] SYN handling on receive is unspecified; JMRI requires none
SerialTrafficController.java:356-360 hunts only for STX; spec p.3:87-88 says
messages start with two SYNs but never says a receiver must require them
(comparison.md §1.6). JMRI's Host RX accepts 0..n SYNs. Combined with the
node-side finding that some nodes DROP frames with a third SYN (Adams), the
safe convention stands: send exactly two, require none.

## Diagnostics & emulation features worth borrowing

For the CMRInet bench instrument, ranked:

1. **Passive metrics listener** (Finding 17): error counters named exactly
   Timeout / Truncated Receive / Truncated Reply / Unrecognized Response /
   Unrecognized Command, plus init count and averaged poll-turnaround time.
   Borrow the taxonomy and the average-over-N smoothing
   (CMRInetMetricsData.java:29-63, 168-185).
2. **Packet monitor** with per-node and per-packet-type filters, NDP-aware I
   decoding, DLE-visible raw mode (Finding 18). The per-node filter bits
   living on the node object make the filter cheap in the hot path.
3. **DiagnosticFrame test suites** (diagnostic/DiagnosticFrame.java:44-47):
   output walking-bit test with observation delay, **loopback wraparound
   test** (write pattern → poll → compare, with invert option for cpNode
   polarity, suspend-on-error + continue), manual init/poll/write-bytes with
   reply byte-count display, and a **Halt Polling** toggle so tests don't
   fight the poll engine (:818-825, backed by `setPollNetwork`,
   SerialTrafficController.java:148-154). The halt-polling primitive is
   essential for any Host that also hosts a test console.
4. **Hex packet generator** (packetgen/SerialPacketGenFrame.java:98-120):
   raw-bytes injection (UA/MT/body as typed; framing/escaping added by the
   normal path only for the header/trailer since the typed bytes go out
   verbatim) plus a one-click poll of an arbitrary address — the minimal
   "speak arbitrary CMRInet" tool.
5. **Per-node poll status** (INIT/POLLING/TIMEOUT/IDLE/ERROR) in a live table
   with per-node polling enable (Finding 17; CMRInetManagerFrame) — the
   at-a-glance network health view.
6. **TCP transport for the same byte protocol** (networkdriver/) — a bench
   instrument that exposes its bus over TCP gets JMRI interop for free.

## Spec ambiguities encountered

Beyond the consolidated list in comparison.md §1 (all confirmed from the Host
side), this review adds Host-specific ones:

- **I/T reply semantics undefined** (Finding 21) — JMRI invents 500 ms / 2 ms
  waits; cpNode SENDEOT exploits the gap.
- **Receiver SYN requirements undefined** (Finding 22) — JMRI requires none.
- **Maximum practical R length**: spec allows 256 data bytes; the reference
  Host tops out at 118 (Finding 4). The spec's "0 to 256" (p.2:75) also never
  says whether that's pre- or post-stuffing (comparison.md §1.7); JMRI's
  reader counts post-unescape bytes against its 120 cap, so on the wire a
  fully escaped 118-byte body (~236 wire bytes) still parses — evidence the
  limit is logical, not wire, length.
- **Poll eligibility**: the spec assumes the Host "polls each active Node"
  (p.2:44) but doesn't define "active"; JMRI's answer (has ≥1 input sensor
  AND autopoll bit) means spec-conformant output-only nodes silently never
  see P (Finding 10).
- **UA range**: JMRI accepts the full 0–127 (SerialNode.java:637-639),
  vs the cpNode family's clamp to 0–64 (comparison.md §2) — the Host is not
  the limiting factor.

## Implications for CMRInet

Where the dominant Host settles comparison.md §2's conventions, copy it;
where JMRI is weak, the node reviews' recommendations stand:

- **Escaping (agrees with §2)**: escape 2/3/16 in every TX body including I;
  never escape 0xFF; two SYNs exactly; single gapless write per frame. Also
  **tolerate raw 0xFF in RX bodies and in I bodies you emit** — JMRI itself
  puts six raw 0xFF pad bytes in every cpNode init.
- **CPNODE I dialect is now fully pinned** (agrees with §1.5, adds detail):
  `'C' dH dL opts1 opts2 NI NO FF FF FF FF FF FF`, 13-byte body, opts bits
  {0:USECMRIX, 1:SENDEOT, 2:USEBCC} default 0, NI/NO include the onboard 2+2.
  A CMRInet **Node-side** implementation must accept exactly this; the
  **Host-side** must emit it (with the pads — fielded cpNode firmware may
  count on body length).
- **Serial (agrees with §2)**: default 8N2 TX / tolerate 8N1, default 19200,
  support the spec's five rates.
- **Timeout numbers to interoperate with JMRI-tuned nodes**: 250 ms poll
  timeout, ≥5-miss re-init threshold, 500 ms post-I settle, ~2 ms post-T gap,
  ~5 ms poll pacing. Make all five configurable per node (JMRI's aren't —
  Finding 9 — and that's its biggest operational weakness; §2's per-node
  timeout recommendation is confirmed, not contradicted).
- **Re-init recipe (agrees with §2)**: on re-init send I, then immediately a
  full T, and invalidate cached input state (Finding 19). Never stop polling
  a silent node, but expose a health flag.
- **Fix what JMRI gets wrong in the RX core** (extends §3's anti-checklist to
  the Host side): verify reply UA (and MT=='R') against the outstanding poll
  before consuming it; size for 256-byte bodies; add an inter-byte timeout
  that abandons partial frames; treat a dangling DLE as a framing error, not
  data; keep an unexpected/misaddressed-reply counter.
- **Poll every configured node** (unlike JMRI, Finding 10) or at least offer
  a keepalive poll for output-only nodes.
- **Bench-instrument feature set**: adopt the metrics taxonomy, per-node
  packet filters, halt-polling primitive, walking-bit + wraparound test
  suites, and raw packet injection (Diagnostics section above). JMRI has no
  node emulator (Finding 20) — CMRInet's emulator/testbed ambition is
  uncontested territory.
- **Conflict with comparison.md §2 worth noting**: none found on wire policy.
  One refinement: §2 says "T on change vs every cycle — verify"; JMRI is
  strictly **T-on-change, full image** with the T inserted ahead of that
  node's next poll. Nodes are therefore NOT conditioned to periodic T
  refresh; a CMRInet option to re-send T periodically would be a
  robustness addition (covers node brownouts between changes) but must
  default off to match observed bus load patterns.
