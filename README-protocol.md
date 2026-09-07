# The CMRInet protocol in brief

Part of the [CMRInet](README.md) guide set. This page is a digest. The
normative wire rules live in the interop profile
([docs/cmrinet-interop-profile-and-errata.md](docs/cmrinet-interop-profile-and-errata.md));
the library architecture lives in
[docs/DESIGN.md](docs/DESIGN.md).

CMRInet (NMRA LCS-9.10.1) moves I/O over a serial bus. A Host
transmits outputs to Nodes and polls them for inputs; each Node
reports its inputs and applies the outputs it receives. Traffic is
half-duplex and Host-driven: a Node speaks only when polled.

## Three layers

1. The data layer — arrays of INPUT and OUTPUT bytes ("images"). The
   bits in these images are the sketch's whole view of the layout.
2. The protocol layer — four packet types exchanged between Host and
   Node.
3. The physical layer — the framing and makeup of the packets, the
   wiring, bit representations, and timing on an RS-422 (4-wire) or
   RS-485 (2-wire) bus.

## Packets

- I (Init) — the Host configures a Node: type, geometry, options.
  Sent before the Node's first poll.
- T (Transmit) — the Host sends the Node's full output image.
- P (Poll) — the Host asks a Node for its inputs.
- R (Response) — the Node replies with its input image. P is the only
  packet a Node answers.

## Framing

Every packet renders on the wire as:

```
SYN/0xFF SYN/0xFF STX/0x02 UA MT body ETX/0x03
```

- The UA byte on the wire is the Unit Address plus 65: UA 0-127
  travels as 65-192.
- Body bytes equal to STX/0x02, ETX/0x03, or DLE/0x10 are escaped
  with a preceding DLE/0x10, in every packet type.
- SYN/0xFF is never escaped, and a receiver never resynchronizes on a
  0xFF data byte inside a body.
- Default serial settings: 28800 baud, 8 data bits, no parity, two
  stop bits.

## Where to read more

- [docs/cmrinet-interop-profile-and-errata.md](docs/cmrinet-interop-profile-and-errata.md)
  — "CMRInet as fielded": the rules that make an implementation
  interoperate with the fielded ecosystem, plus the proposed
  LCS-9.10.1 errata, each cited to its evidence.
- [docs/DESIGN.md](docs/DESIGN.md) — this library's architecture and
  decisions D1-D17: the image and packet seams, naming grammar, the
  health model, conformance.
- [README-references.md](README-references.md) — the spec itself and
  the field literature.
