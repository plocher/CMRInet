# CMRInet

CMRInet is an Arduino library for the NMRA CMRInet protocol
(LCS-9.10.1): layout I/O over a serial bus. A Host transmits outputs
to Nodes and polls them for inputs; each Node reports its inputs and
applies the outputs it receives. This library implements both roles —
Host and Node engines over a shared protocol codec and pluggable
transports. The Node software works with JMRI as the Host.


In the repo: 
- the `CMRIHost` and `CMRINode` engines
- tutorial sketches `SimpleNode`, `SimpleHost`
- a full-featured `XiaoNode` (I2C expanders, OLED, WiFi OTA)
- bench tooling (`TracerHost`, `TracerNode`, `XiaoSniffer`, and
- a desktop Host.

CMRInet implements a three layer architecture: 
1. data (INPUT/OUTPUT byte images), 
2. protocol (I/T/P/R packets), and
3. physical (framing, wiring, timing). 

The Host and Node sketches touch network configuration (node addresses, types...) and the data layer; the CMRInet library does the rest.

- CMRI Bus: 28800 baud, 8N2, RS-422 (4-wire) or RS-485 (2-wire). Host T± →
  Node R±, Host R± → Node T±; Nodes daisy-chain straight through, plus
  to plus, minus to minus.
- Status: v0.1.0, MIT license. The Host and Node engines are complete;
  desktop unit tests cover the codec, both engines, and the
  transports; the example sketches run on cpNode-Xiao hardware.

## Guides

- [Installation](README-install.md) — install the board package,
  the libraries, and CMRInet itself.
- [Hardware Details](README-hardware.md) — the cpNode-Xiao and
  cpNode-IOX boards, pinouts, and bus wiring topologies.
- [Tutorials](README-tutorials.md) — run the examples, Node
  first, and troubleshoot the bus.
- [API Documentation](README-api.md) — the Host and Node API surface.
- [CMRInet Protocol Details](README-protocol.md) — the protocol in brief:
  layers, packets, framing.
- [References](README-references.md) — the spec, the errata,
  JMRI, and other field literature.

## Dependencies

- Boards: the Espressif `esp32` board package. The examples run on the
  Seeed Studio XIAO ESP32C6 (cpNode-Xiao). The engines and codec are
  architecture-independent — the desktop unit tests compile them
  natively.
- Optional, for the OLED examples: the `Adafruit SSD1306` and
  `Adafruit GFX` libraries.
- `ArduinoOTA` ships with the ESP32 core; you do not install it.
- For Nodes: MCP23017-based 16-bit I2C expanders (cpNode-IOX, MRCS
  IOX-16/-32, or generic breakout boards), up to 8 per Node.

## Installation

Clone the library into your Arduino libraries folder (or download the
ZIP and drop the folder there):
```
git clone https://github.com/plocher/CMRInet ~/Documents/Arduino/libraries/CMRInet
```
Restart the IDE. Full steps, including the board package and
verification are in the [Installation Guide](README-install.md).

## Basic usage

### Nodes
A minimal Node: report one input bit, drive one output bit.
The `examples/SimpleNode` sketch does exactly this.
```cpp
#include "CMRInet.h"               // CMRINode, CMRINodeConfig, IOBuffer
#include "transport/serial.h"      // SerialCMRITransport
#include "transport/serialESP32.h" // Esp32SerialPort

CMRInet::Esp32SerialPort     port(Serial1, /* TXEN */ D3, /* Baud */ 28800, RX, TX);
CMRInet::SerialCMRITransport transport(port);
CMRInet::CMRINode            node(transport);

// onPack callback on POLL receipt: read the switches and sensors
void packInputs(CMRInet::IOBuffer& ib) {
  ib.setBit(0, 2, (digitalRead(D2) == LOW));  // active-low button
  // RESPONSE is automatically sent
}

// onUnpack callback on TRANSMIT receipt:  write outputs.
void unpackOutputs(CMRInet::IOBuffer& ob) {
  digitalWrite(LED_BUILTIN, ob.getBit(0, 1) ? HIGH : LOW);
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(D2, INPUT_PULLUP);

  CMRInet::CMRINodeConfig cfg;
  cfg.ua          = 30;
  cfg.nodeType    = 'C';
  cfg.inputBytes  = 2;
  cfg.outputBytes = 2;
  node.config(cfg);
  node.onPack(packInputs);
  node.onUnpack(unpackOutputs);
  node.begin();
}

void loop() {
  node.tick(millis());  // the non-blocking protocol engine
}
```

### Hosts
The Host side is the other side of this protocol conversation.
It uses `host.addRemoteNode(...)`, `host.node(UA)->setOutputBit(...)` 
and `->inputBit(...)` through node handles, and operates the
protocol engine with `host.begin()` in `setup()` and `host.tick(millis())` in
`loop()`. 

While the Node example is a straightforward **read and write on command** loop, 
Hosts are necessarily more complicated because they also implement policy.
Bruce's various Model Railroad Handbooks and CMRI User Guides are in-depth dives
into the art of composing a layout out of logical building blocks:
> if track circuit X is occupied  
> then signal Y should be STOP  
> and turnout Z must be prevented from being thrown out from under the train  

Much of JMRI is devoted to exposing these policies through sensor and turnout
tables, panels, operations, logix and the like.

The `examples/SimpleHost/` sketch implements a simplistic "testbed" policy of 
blinking a bank of outputs and listening for a single pushbutton input.

Details: [API Documentation](README-api.md).

## Examples

`examples/` holds six sketches; they appear under File > Examples >
CMRInet:

- `SimpleNode` — the front-door tutorial: the onboard LED and
  pushbutton, with JMRI as the Host.
- `SimpleHost` — polls a table of remote Nodes, shows each Node's
  health on an OLED, and runs demo behavior services.
- `XiaoNode` — the production Node: up to 8 MCP23017 I2C expanders,
  OLED I/O grid, WiFi OTA firmware updates.
- `TracerHost` and `TracerNode` — the instrumented bench pair used by
  this repo's validation tooling.
- `XiaoSniffer` — RS-422/RS-485 bus sniffer / data logger.

Step-by-step walkthroughs and troubleshooting:
[README-tutorials.md](README-tutorials.md).

## Documents

- References — [README-references.md](README-references.md): the NMRA
  spec, the interop profile and errata, JMRI's CMRI pages, the field
  literature, node-type geometry.
- Design — [docs/DESIGN.md](docs/DESIGN.md) (architecture, decisions
  D1-D17) and [docs/adr/](docs/adr/). Developers working in this repo
  start at [AGENTS.md](AGENTS.md).
- History and evidence — [docs/research/](docs/research/) (seven
  adversarial reviews of fielded implementations, plus the
  [comparison.md](docs/research/comparison.md) synthesis),
  [docs/CHANGELOG.md](docs/CHANGELOG.md), and the bench-findings notes
  under [docs/](docs/).

## Repository layout

- `src/` — the packet codec, the Host and Node engines, the handle
  types, and the shared testbed shell.
- `src/transport/` — the transports. `mock.h` is the test double.
  `serial.h` drives the bus. `serialESP32.h` adds the ESP32 hardware
  transmit drain. `serialPort.h` and `serialStream.h` are the
  byte-port seam, and `CMRITransport.h` carries it. A sketch includes
  the transport it needs.
- `examples/` — the six sketches above.
- `tests/` — desktop unit tests. They do not depend on Arduino.
- `extras/bench/` — bus probe scripts and the single-use bench jigs.
  See its own README.
- `extras/desktop/` — the desktop Host tracer: a CMRIHost over a POSIX
  serial port, driven by the same testbed shell as TracerHost.

## Developers

`make check` runs all three compile-time gates: the desktop unit
tests, the desktop tracer build, and the example-sketch warning gate.
[docs/sketch-warning-gate.md](docs/sketch-warning-gate.md) explains
why the sketch gate runs apart from the build.

Work on a feature branch and open a pull request. Commit messages
follow the conventional-commit style that the history uses.

## License

MIT. See [LICENSE](LICENSE).
