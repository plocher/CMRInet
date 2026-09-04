# Node-type INIT body layouts (evidence pack)

Status: working notes for Host typed INIT (plan: Host node types and
dynamic membership).
Audience: implementers of `CMRIHost` I-body builders and golden tests.

## Conflict rule

Same doctrine as `docs/cmrinet-interop-profile-and-errata.md`:

- When **LCS-9.10.1** text and the **SPS QBASIC lineage** disagree on
  classic (`M`/`N`/`X`) bodies, record both; **fielded wire bytes follow
  SPS** unless a later erratum says otherwise.
- **CPNODE `'C'`** is undefined in the spec body layout; fielded wire
  bytes follow the **JMRI dialect** (interop E3).

## NDP character map (fielded)

| NDP | Code | Node family | Primary source |
| --- | --- | --- | --- |
| `'C'` | 67 | CPNODE (cpNode) | JMRI `SerialNode`; interop E3 |
| `'M'` | 77 | SMINI | SPS `INIT:`; JMRI SMINI |
| `'N'` | 78 | USIC | SPS `INITUSIC:`; JMRI USIC |
| `'X'` | 88 | SUSIC | SPS `INITUSIC:`; JMRI SUSIC |
| `'O'` | 79 | CPMEGA (JMRI extension) | JMRI only; **not** in LCS-9.10.1 |

**Do not swap M/N.** Older plan drafts that mapped SMINI→`'N'` and
USIC→`'M'` were wrong relative to SPS and JMRI.

Sources:

- `docs/ChubbBooks/Original_QBASIC_Files/GREEN_BOOK_V3.0_DISK/QBPGRM/SPSQBG.bas`
  `INIT:` (~L27+): accepts NDP$ `"M"`, `"N"`, or `"X"`; `"M"` branches to
  SMINI checks / `INITSMINI:`; `"N"`/`"X"` to USIC/SUSIC path /
  `INITUSIC:`.
- `docs/research/review-JMRI-cmri-host.md` (I-message contents): SMINI
  `'M'`, USIC `'N'`, SUSIC `'X'`, CPNODE `'C'`, CPMEGA `'O'`.
- `docs/lcs-9.10.1_cmrinet_v1.1.pdf` — names NDP families and classic
  NS/CT semantics; defers card-table detail to Chubb manuals.

## Shared classic header (SPS / JMRI)

For NDP `'M'`, `'N'`, `'X'`, the I **data body** begins:

| Offset | Field | Notes |
| --- | --- | --- |
| 0 | NDP | ASCII `'M'` / `'N'` / `'X'` |
| 1 | dH | `transmissionDelay / 256` (10 µs units) |
| 2 | dL | `transmissionDelay % 256` |
| 3 | NS | type-specific (below) |
| 4… | CT… | type-specific |

SPS packing (`SPSQBG.bas` ~L251–L283):

```
MT = ASC("I")
OB(1) = ASC(NDP$)
OB(2) = INT(DL / 256)      ' dH
OB(3) = DL - OB(2)*256     ' dL
OB(4) = NS
LM = 4
' then append CT bytes; LM tracks length
```

dH/dL are Host policy (interop E4). Modern Hosts send 0/0.

All body bytes equal to STX/ETX/DLE (2/3/16) are DLE-escaped on the wire
(E1); 0xFF is never escaped.

## SMINI (`'M'`)

**Geometry (SPS checks):** NI must be 3, NO must be 6 (input/output
*ports* at the classic 8-bit port size → **3 input bytes, 6 output
bytes** for Host image sizes).

**NS:** number of 2-lead searchlight (yellow-oscillate) pairs; range
0…24 (SPS).

**CT bytes:**

- If **NS == 0**: **no CT bytes**. Body length = 4:
  `<'M'> <dH> <dL> <0>`.
- If **NS > 0**: exactly **6** CT bytes follow — a 48-bit LSB-first mask
  over the 6 output ports marking both bits of each pair (JMRI
  `SerialNode`; SPS loads `CT(1)..CT(6)`). Body length = 10.

SPS: `INITSMINI:` — `IF NS = 0 THEN GOTO TXMSG` else `FOR I = 1 TO 6`
load CT.

JMRI: `docs/research/review-JMRI-cmri-host.md` SMINI bullet; tests cite
User-Manual B10 example.

## USIC (`'N'`) and SUSIC (`'X'`)

**I-body shape is the same** for `'N'` and `'X'`. The letter selects
bits-per-card (24 vs 32) for **geometry / CT validation**, not a
different header layout.

**NS:** number of 4-card sets, `ceil(totalCards/4)`, range 1…16 (SPS).

**CT bytes:** exactly **NS** bytes. Each CT packs four card slots in
base-4 (I=1, O=2, X=0) with weights 1/4/16/64 — spec Table 1 / JMRI
`SerialNode` / SPS SUSIC whitelist.

Body length = `4 + NS`.

**Host image sizes** are **not** carried in the classic I body. They are
derived from the card map × bits-per-card (or declared explicitly and
cross-checked against CT). SPS cross-checks counted I/O ports against
NI/NO; see QBASIC review F25/F16.

## CPNODE (`'C'`) — JMRI dialect (interop E3)

13-byte body (NDP + 12 payload bytes as JMRI `INITMSGLEN=12`):

| Offset | Field |
| --- | --- |
| 0 | `'C'` |
| 1 | dH |
| 2 | dL |
| 3 | opts1 (bit0 USECMRIX, bit1 SENDEOT, bit2 USEBCC; default 0) |
| 4 | opts2 (reserved, default 0) |
| 5 | NI (input **bytes**, including onboard) |
| 6 | NO (output **bytes**, including onboard) |
| 7–12 | six raw `0xFF` pad bytes (never DLE-escaped) |

Sources: interop E3; `docs/research/review-JMRI-cmri-host.md`; current
`CMRIHost::buildInitPacket_` (opts historically hardwired 0).

## CPMEGA (`'O'`) — out of scope for first builders

Same body shape as CPNODE in JMRI; not in LCS-9.10.1. Defer until a
consumer needs it; do not silently alias to `'C'`.

## Golden vectors (logical body, pre-escape)

### CPNODE defaults (today’s Host)

UA irrelevant to body. dH=dL=0, opts=0, NI=2, NO=0:

```
43 00 00 00 00 02 00 FF FF FF FF FF FF
```

(`'C'=0x43`.)

### CPNODE with delay 2000 (JMRI test 0x07,0xD0)

NI=2, NO=3, opts=0:

```
43 07 D0 00 00 02 03 FF FF FF FF FF FF
```

### SMINI, no signals

```
4D 00 00 00
```

(`'M'=0x4D`, NS=0.)

### SMINI with NS>0

Header `4D 00 00 <NS>` + six CT bytes (exact CT values from JMRI B10 /
SPS whitelist when implementing tests).

### SUSIC example shape

```
58 <dH> <dL> <NS> <CT1> … <CT_NS>
```

(`'X'=0x58`.)

## Builder contract (implementation)

Pure functions: type + init fields + (dH,dL) → body bytes + length.
Host `buildInitPacket_` only copies the result into the outbound packet.
Add-time validation rejects illegal NDP, SMINI NI/NO ≠ 3/6, NS ranges,
and CT/geometry mismatches worth checking on the embedded profile.

## Source index

| Artifact | Path |
| --- | --- |
| Formal spec | `docs/lcs-9.10.1_cmrinet_v1.1.pdf` |
| Interop / errata | `docs/cmrinet-interop-profile-and-errata.md` |
| SPS INIT | `docs/ChubbBooks/Original_QBASIC_Files/GREEN_BOOK_V3.0_DISK/QBPGRM/SPSQBG.bas` |
| SPS (alt) | `.../SPSQBC.bas` |
| UM chapters | `docs/ChubbBooks/CMRI_Users_Manual_v3.2/chapters/CHG04_SMINI_*.pdf`, `CHG10_SUSIC_*.pdf`, `APP-B_Serial_Protocol_Subroutines_-_QB_Version_*.pdf` |
| JMRI review | `docs/research/review-JMRI-cmri-host.md` |
| QBASIC CTC review | `docs/research/review-QBASIC-CTC-host.md` |
