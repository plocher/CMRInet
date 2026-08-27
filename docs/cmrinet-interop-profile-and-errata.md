# CMRInet as Fielded: Interop Profile and Errata for LCS-9.10.1

Status: working draft for review.
Version: 1.1 (bump when any rule or erratum changes; `// VALIDATION:`
tags in code cite this version — see
`docs/agents/validation-comments.md`).
Audience: LCS-9.10.1 authors, JMRI maintainers, and implementers of
CMRInet Hosts and Nodes.
Date: 2026-08-15.

Change log:
- v1.1 (2026-08-15): rule 2.2.6 extended with the nominal-observation /
  max-gap-watermark receive clause (issue #26). The abort obligation is
  unchanged; the addition is a MAY for implementers. v1.0 tags in code
  are re-stamped to v1.1.
- v1.0 (2026-08-12): initial working draft.

## Purpose

NMRA LCS-9.10.1 v1.1 (December 2014) defines the CMRInet protocol. The
spec text and the fielded ecosystem disagree on several points. In two
cases the spec contradicts itself. This paper does two things:

1. Part 1 records each spec defect and proposes replacement wording.
2. Part 2 states the rules that make an implementation interoperate
   with the fielded ecosystem today, before any spec revision lands.

Every claim in this paper traces to an adversarial source review of a
fielded implementation. The reviews cite the spec by page and the code
by file and line.

## Evidence base

Seven implementations were reviewed against LCS-9.10.1 v1.1. The full
reviews are in `docs/research/` in the CMRInet repository.

Node (slave) implementations:
- ArduinoCMRI v1.5 and v1.7.0 (Michael Adams) — `review-CMRI-adams.md`
- cpNode library — `review-cpNode-simple.md`
- cpCMRI library — `review-cpCMRI-enhanced.md`
- MRCS_cpNode_kernel v1.6 — `review-MRCS-kernel.md`

Host (master) implementations:
- JMRI CMRI stack, current — `review-JMRI-cmri-host.md`
- SBHRS CMRI-Controller, 1998-2014, pre-JMRI — `review-CMRI-Controller-host.md`
- SoCal Ry CTC machine on Chubb's Serial Protocol Subroutines (SPS,
  2003) — `review-QBASIC-CTC-host.md`

The SPS code is the lineage of the spec's own normative reference [1].
Where the spec text and the SPS behavior disagree, the fielded
ecosystem follows the SPS behavior.

`docs/research/comparison.md` holds the cross-review synthesis.

## Terms

This paper uses the spec's terms: Host (master) and Node (slave).
The key words MUST, MUST NOT, SHOULD, and MAY mark requirement levels.
In Part 2, MUST means: required to interoperate with fielded equipment.

Protocol characters: SYN/0xFF, STX/0x02, ETX/0x03, DLE/0x10. This
paper always names a protocol character together with its hex value.
A node's Unit Address (UA) is an ordinal in the range 0..127. The
wire UA is the byte transmitted on the serial line: UA + 65 (range
65..192).

# Part 1 — Errata

Each item gives the spec text, the defect, the fielded resolution, and
proposed replacement wording.

## E1. DLE scope excludes the I message — the I message is not parseable

Spec text: "Protocol management software must insert a DLE in front of
any of these data values when forming a Transmit Data or Receive Data
message" (p.5-6 §D.a). The I message is not listed.

Defect: I-message bodies legally contain the values STX/0x02, ETX/0x03,
and DLE/0x10. The dL byte holds any 8-bit value. Table 1 (p.7) assigns
CT value 0x02 (the STX value) to card set OXXX. NS can be 0x02 or 0x03.
A byte-scanning receiver cannot tell an unescaped CT byte of 0x03 from
the ETX/0x03 trailer. Non-addressed Nodes, told to "discard all the
remaining bytes until an ETX is seen" (p.5), stop early on the same
bytes. The spec's framing rules and its I-message definition contradict
each other.

Fielded resolution: every reviewed Host escapes I bodies. The SPS
reference code has always done so (review-QBASIC-CTC-host.md, F1). The
SBHRS Host did so against real USIC hardware for years
(review-CMRI-Controller-host.md, Finding 2). JMRI does so and asserts it
in a unit test (review-JMRI-cmri-host.md, Finding 1). Three of the four
reviewed Nodes unescape all bodies and mis-parse unescaped I bodies.

Proposed wording: "Protocol management software must insert a DLE/0x10
in front of any data value equal to STX/0x02, ETX/0x03, or DLE/0x10 in
the body of every message type. The receiver, when it sees a DLE/0x10
in a message body, ignores the DLE and takes the next byte as data."

## E2. Stop bits: the spec says one, the reference lineage uses two

Spec text: "CMRInet character framing consists of 10 bits; 1 Start bit,
8 Data bits, 1 Stop bit" (p.2 §A).

Defect: the SPS reference code programs the UART for two stop bits
(LCR = 7) and recommends two stop bits at 57600 and above. Its comment
sanctions one stop bit only at 28800 and below
(review-QBASIC-CTC-host.md, F2). JMRI uses two stop bits by constant
(review-JMRI-cmri-host.md, Finding 2). MRCS cpNode hardware and the
SBHRS Host use one stop bit. The installed base is split. A transmitter
that uses two stop bits is compatible with a one-stop-bit receiver,
because the extra stop bit reads as idle line. The reverse direction can
produce framing errors on back-to-back bytes.

Proposed wording: "Character framing is 8 data bits, no parity, and one
or two stop bits. A receiver must accept either. A transmitter should
use two stop bits at speeds of 57600 BPS and above."

## E3. The C (CPNODE) I-message body is undefined

Spec text: NDP 'C' is named (p.6, "CPNODE with 16 to 144 input/outputs
using 8 bit cards") but NS and CT semantics are defined only for N, X,
and M Nodes.

Defect: the spec names a Node type without defining its initialization
body. Every CPNODE implementation uses a private dialect inherited from
JMRI.

Fielded resolution: JMRI sends this 13-byte body for NDP 'C'
(review-JMRI-cmri-host.md, Host wire-behavior facts):

    <'C'> <dH> <dL> <opts1> <opts2> <NI> <NO> <0xFF x6>

opts1 bit 0 = USECMRIX, bit 1 = SENDEOT, bit 2 = USEBCC, all default 0.
opts2 is reserved. NI and NO count 8-bit input and output cards and
include the two onboard input and two onboard output ports. The six
pad bytes have the SYN value (0xFF) and are sent raw, never escaped.
All body bytes equal to STX/0x02, ETX/0x03, or DLE/0x10 are DLE-escaped
(per E1).

Proposed wording: adopt the JMRI dialect above as the normative C-type
I-message body. State that the pad bytes are reserved and transmitted
as raw 0xFF.

## E4. Transmission delay (dH/dL) granularity is undefined

Spec text: "Each unit of transmission delay represents 10 microseconds.
... A non-zero transmission delay value will cause SMINI, SUSIC and
USIC Nodes to delay between transmissions to the Host" (p.6).

Defect: "between transmissions" does not say per character or per
message. The spec also omits CPNODE from the list, so C-type obligation
is unknown. Fielded Node code applies the delay per transmitted
character, which follows classic practice. AVR-based Nodes mis-honor
delays above 16383 microseconds because of a library limit. The field
is not vestigial: the SBHRS Host shipped dH/dL = 0/200 (2 ms per
character) to protect a slow DOS-era receiver
(review-CMRI-Controller-host.md, Finding 3).

Proposed wording: "The transmission delay is the minimum idle time the
Node inserts after each transmitted character. One unit equals 10
microseconds. Modern Hosts should send zero. All Node types must accept
the field. A Node may cap the honored delay and must document its cap."

## E5. SYN handling on receive is undefined

Spec text: "A message starts with two SYN ... characters" (p.3). The
spec does not say whether a receiver must require them, or whether more
than two are legal.

Fielded facts: all three reviewed Hosts ignore SYN/0xFF on receive and
hunt for a bare STX/0x02. Most Nodes do the same. One fielded Node
(ArduinoCMRI through v1.7.0) drops the frame when a third SYN/0xFF
precedes STX/0x02 (review-CMRI-adams.md). Data bytes with the SYN value
(0xFF) are legal in bodies and are never escaped. JMRI transmits six
raw 0xFF bytes inside every C-type I body (E3), so a receiver must not
resynchronize on SYN/0xFF inside a frame.

Proposed wording: "A transmitter must send exactly two SYN/0xFF
characters before STX/0x02. A receiver must not require SYN characters
and must not treat a 0xFF data byte inside a message body as
synchronization."

## E6. No error-recovery semantics for either side

Spec text: the only defined recovery is the Host poll timeout: "the
Host software handles a timeout error, and the next Node in the poll
list is polled" (p.8). The timeout has no value and no unit.

Defect: the spec is silent on truncated frames, unknown message types,
oversized bodies, inter-byte gaps, and Node-side resynchronization.
Every implementation invents its own recovery. The reviews show that
this is where the bugs cluster on both sides: indefinite blocking waits,
buffer overruns, and partial frames applied to layout outputs
(comparison.md §3, §6).

Proposed wording (minimum): "A receiver must abandon a partial message
and return to hunting for a message start when the gap between bytes
exceeds a locally defined limit. A receiver must discard, not act on, a
message that ends without ETX/0x03 or that exceeds the defined body
length. A Node that receives an unknown message type addressed to it
must discard the message through its ETX/0x03."

## E7. Body length "0 to 256" does not state pre- or post-escape counting

Spec text: "The message body (data) is 0 to 256 data bytes in length"
(p.2).

Defect: with DLE stuffing, a 256-byte logical body can occupy up to 512
bytes on the wire. The spec never says which count the limit applies
to. JMRI counts logical bytes and caps replies at 118 data bytes, below
the spec maximum (review-JMRI-cmri-host.md, Finding 4).

Proposed wording: "The body length limit of 256 counts data bytes after
DLE/0x10 removal. Receivers must size buffers for 256 data bytes and
must accept the corresponding worst-case wire length."

Informative note for the revision: the dominant fielded Host truncates
replies longer than 118 data bytes. Until that changes, a Node
configuration that reports more than 118 input bytes does not
interoperate with JMRI.

Fielded reality check: the largest Node hardware ever built is a
SUSIC backplane with two 16-bay card racks — 32 I/O cards total. With
32-bit input cards in every slot, that is 128 data bytes, near 1000
inputs. No fielded frame approaches 256 bytes, and JMRI's 118-byte
cap never bit in practice. The cpNode/SMINI move to small distributed
Nodes made such monolithic card cages obsolete (and ended their
wiring jungles), so the fielded maximum will not grow. The 256
ceiling matters only for conformance testing and as headroom for a
possible future extension that carries more than bits — enums or
bounded fixed-point values, conceptually like CAN bus signal packing.

## E8. Reply expectations for I and T are undefined

Spec text: the spec defines a reply (R) only for P (p.7-8).

Defect: the spec never states that I and T receive no reply. JMRI
resolves this with fixed waits: 500 ms after I, about 2 ms after T
(review-JMRI-cmri-host.md, Finding 21). The cpNode SENDEOT option makes
a Node transmit an 'E' frame after T, a message type absent from the
spec. JMRI accepts it only because its engine accepts any frame as "the
reply."

Proposed wording: "Nodes do not reply to I or T messages unless a
documented option enables an acknowledgment. A Host must not require a
reply to I or T." A revision should either define the EOT ('E')
acknowledgment or deprecate it.

## E9. Smaller items for the same revision

- "Active Node" (p.2) is undefined. JMRI polls only Nodes with at least
  one registered input. Output-only Nodes never receive P and never
  recover from a power cycle without operator action
  (review-JMRI-cmri-host.md, Finding 10). Define poll eligibility, or
  state that Hosts should poll every configured Node.
- UA range: the spec allows 0-127. The cpNode family clamps addresses
  to 0-64 and silently changes out-of-range values to 64
  (review-cpNode-simple.md). State that Nodes must reject, not remap,
  out-of-range addresses.
- Addressed-Node tolerance: the spec's discard-to-ETX rule covers only
  non-addressed Nodes (p.5). State whether an addressed Node must
  tolerate trailing bytes between its parsed body and ETX/0x03. Fielded
  classic hardware tolerated them (review-CMRI-Controller-host.md,
  Finding 1/20).
- JMRI extensions in the wild: NDP 'O' (CPMEGA) and message types E, Q,
  D, W, A, C, M appear on fielded networks and are absent from the
  spec (review-JMRI-cmri-host.md, Finding 3). Document or reserve them.

# Part 2 — Interop profile

These rules make a new implementation interoperate with the fielded
ecosystem as it exists today. They assume no spec revision.

## 2.1 Rules for every transmitter (Host or Node)

1. Send exactly two SYN/0xFF characters, then STX/0x02, UA, MT, body,
   ETX/0x03.
2. DLE-escape every body byte equal to STX/0x02, ETX/0x03, or DLE/0x10,
   in every message type, including I.
3. Never escape SYN/0xFF.
4. Do not emit bytes between frames.
5. Prefer a single buffered write per frame. Gapless frames protect
   Nodes with known parser defects. Do not depend on gaps for pacing.
6. Size the transmit staging buffer for full escaping: two times the
   body length, plus header and trailer. Two reviewed Hosts have latent
   overflows here (review-CMRI-Controller-host.md Finding 4,
   review-QBASIC-CTC-host.md F23).

## 2.2 Rules for every receiver (Host or Node)

1. Hunt for STX/0x02. Do not require SYN/0xFF. Do not count SYNs
   against a frame.
2. Process DLE/0x10 before the STX/0x02 and ETX/0x03 tests. After a
   DLE, take the next byte as data with no interpretation, including in
   the last body position.
3. Treat 0xFF (the SYN value) in a body as data. Never resynchronize
   on SYN/0xFF mid-frame.
4. Reset the body index when STX/0x02 is found. Store no bytes before
   STX/0x02.
5. Bound-check before every buffer store. Size for 256 data bytes.
6. Abandon a partial frame when the inter-byte gap exceeds the abort
   limit. Two to three character times is a reasonable abort default for
   a clean UART; deployed Hosts run a more tolerant abort limit (tens of
   milliseconds) with the reply-gate timeout as the truncation backstop.
   Exception: a conformance-grade Node receiver should tolerate arbitrary
   gaps (abort disabled), because the reference Host lineage transmitted
   with interpreter-scale gaps between bytes
   (review-QBASIC-CTC-host.md, F5).

   A receiver MAY keep a second, lower nominal threshold below the abort
   limit and record inter-byte gaps that exceed it without abandoning the
   frame. This separates "took longer than expected" (a non-fatal
   annotation) from "took so long I gave up" (the fatal abort), so
   marginal wiring, wobbly nodes, and host tick stalls are visible in
   telemetry without failing exchanges. The nominal threshold is a local
   guess about the medium; it cannot root-cause a fault by itself, but it
   feeds systemic analysis. A max-gap watermark records the largest
   inter-byte gap seen, including the gap that triggers an abort. The
   abort limit and the nominal threshold are independent: disabling the
   abort (conformance) leaves observation on; disabling observation
   leaves the abort on.
7. Treat a frame that ends in DLE/0x10, or that ends without ETX/0x03,
   as an error. Discard it. Never act on a partial body.
8. Parse into a staging buffer. Commit to the application only on a
   valid ETX/0x03. The QBASIC review documents the hazard: a truncated
   frame committed early reported an occupied block as vacant
   (review-QBASIC-CTC-host.md, F15).
9. Normalize received bytes to unsigned 8-bit values at the read. Do
   not compare protocol constants through signed char.

## 2.3 Host rules

Transmit policy:
1. Send I to a Node before its first P, and send a full T immediately
   after every I. The immediate T repairs a known Node defect where the
   I body transiently clobbers output state (review-CMRI-adams.md,
   v1.7.0 delta).
2. Send the full output image in every T frame. Never send a short or
   partial image. Fielded Nodes fill missing bytes from stale memory.
3. Send dH/dL = 0 unless a specific Node needs pacing. Expose the field
   per Node.
4. Support UA 0-127. Warn on addresses above 64, because the cpNode
   family cannot use them.

Receive policy:
5. Verify that a reply's UA matches the outstanding poll and that its
   MT is 'R'. Count and discard everything else. JMRI omits this check
   and decodes echoed traffic as sensor data
   (review-JMRI-cmri-host.md, Findings 5-6). The classic Hosts got this
   right.
6. Default the per-node reply capacity to 118 data bytes — JMRI's
   exact ceiling (JMRI caps a reply at 120 elements including UA and
   MT). Any geometry a Host accepts under this default also works
   under JMRI, so no configuration can pass on the bench and silently
   fail under the dominant fielded Host. Make the ceiling a
   compile-time option: 128 covers the largest Node ever fielded (a
   full SUSIC backplane: two 16-bay racks, 32 cards of 32-bit inputs)
   for beyond-JMRI bench work; 256 is the protocol ceiling, useful
   only for conformance testing (E7).

Scheduling and recovery:
7. Use wall-clock timeouts, per byte and per message, configurable per
   Node. Defaults that match JMRI-tuned Nodes: 250 ms poll timeout,
   500 ms settle after I, about 2 ms after T, about 5 ms poll pacing.
8. Budget reply latency per Node version. ArduinoCMRI v1.5 Nodes delay
   50 ms before every reply. Later versions reply in about 50
   microseconds. Node dH/dL adds per-character delay.
9. Do not retransmit a timed-out message. Count the miss and poll the
   next Node.
10. After more than 5 consecutive poll misses: re-send I, then a full
    T, and invalidate cached input state for that Node. Keep polling a
    silent Node forever, and expose its health state to the application.
11. Cap retries on malformed replies. The reference Host re-polls
    without bound and a persistent corrupter pins the whole network
    (review-QBASIC-CTC-host.md, F14).
12. Expose per-Node freshness as API state. Never hold stale inputs
    silently.
13. Poll every configured Node, or provide a keepalive poll for
    output-only Nodes.

RS-485 line discipline, where the converter does not manage direction:
14. Assert TXEN, write the frame, flush until the last byte leaves the
    shift register, then drop TXEN at once.
15. Expect a fast Node to begin its reply while the Host's ETX/0x03
    still drains. Drop the driver promptly.

## 2.4 Node rules

1. Answer only frames whose UA matches. For all other traffic, discard
   through ETX/0x03 with DLE/0x10 processing active during the discard.
   A DLE-blind discard stops early inside another Node's body.
2. Unescape DLE/0x10 in every message type, including I (per E1).
3. Tolerate a trailing byte between the parsed I body and ETX/0x03. At
   least one fielded Host transmits one
   (review-CMRI-Controller-host.md, Finding 1).
4. Escape STX/0x02, ETX/0x03, and DLE/0x10 in R replies. The reference
   Host rejects replies with unescaped protocol characters, and that
   check trained the ecosystem (review-QBASIC-CTC-host.md, F26).
5. Reply to P promptly. Modern Hosts allow 250 ms. Do not insert fixed
   delays.
6. Keep the reported input size at or below 118 bytes to interoperate
   with JMRI (E7).
7. Do not reply to I or T unless SENDEOT is configured.
8. Validate configured geometry against I-message NI/NO where possible,
   and surface mismatches. No fielded C-type Node does this today, and
   misconfiguration flows silently.
9. Expect re-initialization at any time. Classic Hosts never re-init,
   but JMRI re-inits after 6 missed polls. Both are fielded behaviors.

## 2.5 Serial configuration

1. Transmit 8N2 by default. Accept 8N1 configuration where a network
   requires it (per E2).
2. Support 9600, 19200, 28800, 57600, and 115200 BPS. JMRI defaults to
   19200.
3. Desktop-side tools on POSIX must configure a fully raw terminal. The
   SBHRS Unix port left IXON enabled, and one XOFF/0x13 data byte froze
   all Host transmit (review-CMRI-Controller-host.md, Finding 7).

## 2.6 Bus topology and echo visibility

1. The fielded norm is 4-wire: one pair carries Host-to-Node traffic
   (the poll pair), the other carries Node-to-Host traffic (the reply
   pair). Most historical deployments, and all current ones known to
   the authors, use this topology.
2. On 4-wire, a Node's receiver sits only on the poll pair. A Node
   never hears its own reply, another Node's reply, or an echo of its
   own transmission. The Host's receiver sits only on the reply pair
   and never hears its own polls.
3. Consequence for receiver hardening: fielded Node receivers have
   never been exercised against 'R' traffic or self-echo. Host receive
   defects stay latent for the same reason. On 4-wire the reply pair
   carries only the polled Node's reply, so the missing JMRI UA/MT
   check (rule 2.3.5) rarely misfires in the field.
4. A 2-wire network, with both directions on one shared pair, is
   electrically possible and historically documented, but rare. On
   2-wire every receiver hears all traffic, including its own
   transmissions. Rules 2.2.1 through 2.2.9, 2.3.5, and 2.4.1 then
   stop being defense in depth and become load-bearing. Treat 2-wire
   as a conformance scenario, not a deployment assumption.

# Open questions for the revision

1. Adopt or deprecate the JMRI protocol extensions: NDP 'O', message
   types E/Q/D/W/A/C/M (E9).
2. Define poll eligibility for output-only Nodes (E9).
3. Set a normative minimum for receiver body capacity, given the JMRI
   118-byte reply ceiling (E7).
4. Decide whether the revision blesses per-character dH/dL for all Node
   types or retires the field (E4).
5. Decide whether Node-side inter-byte timeouts become normative, given
   that the reference lineage transmitted with gaps (E6, profile 2.2.6).
6. Decide the fate of body lengths above the fielded maximum of 128
   data bytes (E7): a revision could reserve longer bodies for a
   typed-payload extension — enums or bounded fixed-point values
   rather than more bits, conceptually like CAN bus signal packing.
7. Define an acknowledgment to I (or T) that carries the Node's
   self-description, so a Host can validate configured geometry against
   the Node's actual hardware at init time. Today I and T expect no
   reply (E8); a geometry mismatch between the Host's configured NI/NO
   and the Node's actual I/O is detectable only at the first P/R
   exchange, and only by the Host (rule 2.4.8: no fielded C-type Node
   validates its own NI/NO). The SimpleHost ergonomics probe (issue #31)
   surfaced this: a misconfigured inputBytes leaves the Node answering
   but the Host rejecting every R, with no diagnostic until the
   rejection reason is inspected on the Host side. An Init ack carrying
   NDP, NI, NO, and card type would let the Host reject or warn at I
   time. This extends the E8 revision thread ("define the EOT
   acknowledgment or deprecate it").
8. Bus discovery via a self-identify message type. A new MT (e.g. 'G')
   the Host sends to a UA, replied to with the Node's self-description
   (NDP, NI, NO, card type, firmware). Enables Host auto-configuration:
   poke UA 0..127, collect self-IDs, and populate the node table without
   manual entry. No fielded Host or Node does this today. Presupposes
   open question 7 (the Node must be able to describe itself) or an
   equivalent self-description reply. Larger lift: a new packet type in
   both directions plus a discovery sequence, and a Node-side
   counterpart no fielded Node has.
