# ADR-0004: Library boundary and transport header packaging
Date: 2026-08-30
Status: Accepted
Related: #9, #13, #14, #74, #75, #76, #77, #78, ADR-0001
Cross-links: `docs/DESIGN.md` D1, D3, D11, D12
## Context
DESIGN framed the product as a layout I/O image service with CMRInet
as its first strategy, and reserved MQTT as both a packet carrier
(D11) and a future engine family (D1 reserves `MQTTHost`). A series of
design conversations about the WiFi-native Xiao and the rich-semantic
MQTT path exposed three questions whose answers together redraw the
boundary:
1. **Where does the semantic/point layer live?** cpCMRI's `ioMap`
   binds named I/O points to byte-of-bits positions and to hardware
   devices. It is protocol-neutral and role-neutral, and a rich-MQTT
   device needs it while wanting nothing else from this library.
2. **Does MQTT-as-carrier have a consumer?** JMRI cannot speak
   CMRI-packets-over-MQTT. A wireless node is better served by TCP,
   which JMRI's `networkdriver` already speaks. Rich MQTT is the
   actual motivation, and it lives in the semantic layer, not here.
3. **What naming and file structure should the transport family use?**
   Transport implementations sat at the top of `src/` with names that
   repeated the qualifier a subdirectory should carry, and the umbrella
   header force-included implementations rather than just the seam.
## Decision
**This library is the CMRI strategy.** Its scope is packets, byte
images, and the two CMRI engines (`CMRIHost`, `CMRINode`), over
pluggable packet carriers. You want this library if you are
implementing CMRInet-compliant Hosts, Nodes, or Gateways.
### Semantic layer relocated
The point/semantic layer (named, typed I/O points; hardware binding;
rich data models) belongs in a sibling library, not here. Nothing would
move there today — the baseline Node and Host both deal in raw byte
images, and cpCMRI's `ioMap` already lives in its own library. The
relocation is a boundary decision, not a migration. The `pack`/`unpack`
seam (function-pointer + context, not cpNode-style globals) is what lets
a future sibling library own both callbacks with the sketch never
touching bytes, so the seam shape is load-bearing for this boundary
rather than stylistic.
A gateway is a `CMRIHost` deployment whose app mirrors images outward
to the sibling library's topics. It is not a new concept or type in
this library — it needs nothing beyond what `SimpleHost` already needs.
### MQTT-as-carrier dropped
D11 is obsolete as written. The packet seam already has three
implementations across two real media (mock, serial, and the planned
TCP carrier), so a third-party implementation of *any* transport serves
the seam-proof role D11 cited MQTT for. MQTT was never special for it.
The whole point of a seam is that you don't have to build every
implementation to know it holds.
### TCP as the alternate carrier
TCP is the carrier that buys JMRI interop today: JMRI's `networkdriver`
speaks CMRI over a raw TCP socket, so a WiFi-native node presenting
frames on a socket is consumable with no JMRI-side work. This is also
the product path for replacing RS-485 wiring with WiFi on the same
hardware — same `CMRINode`, same sketch, carrier swapped.
On a point-to-point socket P is media-access control for a party line
that no longer exists, so polling becomes pure overhead. DESIGN already
calls this "vestigial but harmless." You pay a round-trip for a packet
carrying no data, and you buy total interop with existing Hosts.
### Transport header packaging
Directory = seam. Filename = discriminator. Generic names live in
subdirectories; top-level headers stay distinctive. Leaf headers pull
their own dependencies so a sketch names only what it chooses:
```cpp
#include "CMRInet.h"                // engines, packets, images, transport seam
#include "transport/serialESP32.h"  // the choice; pulls serial.h itself
```
The umbrella (`CMRInet.h`) carries the seam plus packet, codec, time,
engines, and handles — but **no implementations**. A sketch that wants
mock or serial includes the leaf header for it.
The generic-vs-distinctive split is not aesthetic. Arduino PR #1853
(merged Feb 2014) made a library whose folder name matches the header
filename win a collision, leaving everything else last-found-wins. The
canonical guidance from that thread is to place headers not meant for
the sketch into subdirectories. `CMRInet.h` is protected by the
folder-name match; `transport.h` would not be, so the seam contract
stays `CMRITransport.h` (distinctive at top level) while the
subdirectory files drop the prefix (`transport/mock.h`,
`transport/serial.h`, `transport/serialESP32.h`).
Type names do not change. File layout and type naming are two separate
naming systems: `CMRInet::SerialCMRITransport transport(port);` is
self-describing at a use site where no path is visible, and a gateway
holding two carriers at once needs distinct names.
## Consequences
Immediate:
- D11 is obsolete; D12 sheds its semantic half (the gateway is a
  `CMRIHost` deployment, and the topic mirror is the sibling library's
  job); D1 stops reserving `MQTTHost` as a future engine here; D3 is
  strengthened: this library *is* the CMRI strategy, and second
  strategies live elsewhere.
- The one-product/two-seams layer model is rescoped: the image seam is
  this library's top edge, not a universal cross-strategy contract. The
  units ladder survives intact.
- Transport headers move into `src/transport/`; the umbrella stops
  including implementations. This is a breaking include-path change for
  any consumer already on the library.
- Backlog simplifies: MQTT carrier tickets close as descoped; JMRI MQTT
  schema and semantic gateway tickets close as belonging to the sibling
  library.
- ADR-0001's deferred question (should `RemoteNodeHandle::wireUA()`
  exist on a strategy-neutral surface?) may now be resolvable: #9 is
  landing and the surface just narrowed to CMRI-only, so
  strategy-neutrality across engines is no longer the concern it was
  framed against.
Deferred:
- The sibling library has no ticket. Nothing would move there today, so
  this ADR alone is enough to stop someone starting it here. A ticket
  is filed when the point layer has a real consumer.
- `transport/tcp.h` and its platform refinements are future work, not
  part of this landing.
## Revisit trigger
Revisit if a rich-semantic MQTT device or a point-layer abstraction
needs to live inside this library to be useful. The boundary was drawn
to keep that out, not to deny it exists — the revisit is "should we
merge the sibling in," not "should we build one here."
