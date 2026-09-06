# CMRInet

A protocol suite for **CMRInet** (NMRA LCS-9.10.1): **Host** and **Node** engines over a shared codec and pluggable transports, plus a bench-instrument emulator. 

Example sketches: 
 - `SimpleHost` - a testbench example that illustrates Node creation    and configuration, with service behaviors that demonstrate output animation and logic that reads inputs and writes outputs.
 - `SimpleNode` - a simple example that works on a stand alone     cpNode-Xiao board, using the built in LED and an onboard pushbutton.
 - `XiaoNode` - a production ready example that supports up to 8x cpNode-IOX, MRCS IOX-16 or (4x) IOX-32 I2C expander boards, an onboard OLED display and WiFi OTA for firmware updates.

Bench test instruments:
 - `TracerHost` and `TracerNode` are instrumented with a USB serial remote command and control protocol, allowing test harness orchestration, monitoring and validation of an attached hardware test bench.
 - `XiaoSniffer` is a cpNode-Xiao set up as a RS422/RS485 to USB serial data logger.  We use two of these on our test bench to capture both the Host commands and the Node's responses, and validate the correct operation and timing of the protocol flow.

Native simulators/emulators:

## Architecture at a glance

This library implements the NMRA CMRInet specification, both Host and Node perspectives. The spec can be seen as having three layers:
1. the data layer - arrays of INPUT and OUTPUT bytes.
2. the serial protocol layer - I, T, P and R packets passed between Hosts and Nodes
3. the physical layer - the framing and makeup of the packets, wiring, bit representations and timing.

After setting up its environment (Host or Node, identity, provisioning...), a sketch deals with the data layer, content freshness and health, leaving the library to handle the mechanics of packet reception and transmission, errro handling, etc.

### As a Host, the core API is 
  - `RemoteNodeHandle`s, one per node, provide access to output writes, input reads, input age, health state, and statistics. 
  - `RemoteNodeState` is strategy-neutral and derived from (liveness, image state, conformance): `UNINITIALIZED`, `ONLINE`, `STALE`, `OFFLINE`, `MISCONFIGURED`,
`DEGRADED`.
  - `host.node(UA)->setOutputBit(byte, bit, value)`
  - `value = host.node(UA)->inputBit(byte, bit)`
### As a Node, the core API is
  - `packInputs(CMRInet::IOBuffer& ib)` and `ib.setBit(byte, bit, value)`
  - `unpackOutputs(CMRInet::IOBuffer& ob)` and `ob.getBit(byte, bit)`

## Getting started

 - `examples/SimpleHost/SimpleHost.ino`. The tutorial example for the Host side. The sketch polls a predefined list of remote nodes, shows each node's health on an OLED, and runs a set of simplistic behavior services: blinking outputs and an output that follows an input.

 - `examples/SinpleNode/SimpleNode.ino`. The tutorial example for the Node side and uses a cpNode-Xiao board with a button and LED.

 - `examples/SinpleNode/XiaoNode.ino`. A full featured CMRInet node for use with a cpNode-Xiao board and cpNode-IOX I2C boards.  It supports the onboard OLED, displaying the I/O state of all the expanders, as well as WiFi OTA for firmware updates.

### You will need:

- A cpNode-Xiao board (Seeed XIAO ESP32-C6 + MAX3491) as Host and/or Node.  JMRI can be used as a Host if all you want to do is try the Node sketch - in which case you will also need a USB to 4-wire RS422 dongle.
- I2C expanders such as the cpNode-IOX, MRCS' IOX-16 and -32, generic 23017 breakout boards, etc.
- The Seeed and Expressif processor packages for the Arduino IDE environment.
- The Adafruit SSD1306 and GFX libraries for the OLED. Undefine `USE_OLED` to compile without this display.
- The ArduinoOTA library if WiFi OTA support is desired.

### Bus Wiring:
 - Host's T± to the Node's R± and 
 - Host's R± to the Node's T±. This is a crossover cable.
 - All the Nodes on the bus are wired in parallel, their T± pairs and R± pairs daisy chained to each other, plus to plus, minus to minus.

### Host side
 - Edit the `nodeTable` of `HostNodeSpec` rows to match your layout:
```
HostNodeSpec nodeTable[] = {
  // UA 30: CPNODE — 2 onboard + 0 IOX bytes in and out
  hostNodeCpnode(30, CpnodeInit(2, 2)),
  // Examples of other types
  hostNodeCpnode(31, CpnodeInit(10, 10)), // + 8 IOX expanders...
  hostNodeSmini(5, CMRInet::SminiInit(/*ns=*/0)),
  hostNodeSusic(10, CMRInet::UsicFamilyInit(/*ns=*/1, /*NI=*/4, /*NO=*/4)),
}
```
 - Upload and open a serial monitor.

### Node side
 - SimpleHost: Set the Node's UA and Edit the onPack and onUnpack routines:
```
  cfg.ua          = 30;
```
```
// The cpNode-Xiao using the CMRInet:XiaoNode firmware has limited 
// onboard I/O:
//
// Byte  Bit  Input            Output
//   0    1    na               Onboard LED
//   0    2    D2 (pushbutton)  na

// onPack callback on POLL receipt: read the switches and sensors
void packInputs(CMRInet::IOBuffer& ib) {
  ib.setBit(0, 2, (digitalRead(D2) == LOW));  // active-low button
  // RESPONSE is automatically sent
}

// onUnpack callback on TRANSMIT receipt:  write outputs.
void unpackOutputs(CMRInet::IOBuffer& ob) {
  digitalWrite(LED_BUILTIN, ob.getBit(0, 1) ? HIGH : LOW);
}
```
 - XiaoHost: Edit the IOX table.  The onPack/onUnpack routines know about this table and automatically handle everything.
```
IOX_Config expanders[] =
#if NODE_ID == 30
{
    { 0x20, IN,     OUT    },
    { 0x21, IN,     OUT    },
    { 0x22, IN,     OUT    },
    { 0x23, OUT,    IN     },
    { 0x24, OUT,    IN     },
    { 0x25, UNUSED, UNUSED },
    { 0x26, UNUSED, UNUSED },
    { 0x27, UNUSED, UNUSED },
};
```

## Documents

- [docs/lcs-9.10.1_cmrinet_v1.1.pdf](docs/lcs-9.10.1_cmrinet_v1.1.pdf)   — the NMRA specification.
- [docs/cmrinet-interop-profile-and-errata.md](docs/cmrinet-interop-profile-and-errata.md)
  — "CMRInet as fielded": normative interop rules and proposed
  LCS-9.10.1 errata, evidence-cited.
- [docs/research/](docs/research/) — eight adversarial reviews of
  fielded implementations (four Node-side, three Host-side) against
  the spec, plus the cross-review synthesis
  ([comparison.md](docs/research/comparison.md)).
- [docs/DESIGN.md](docs/DESIGN.md) — architecture documentation for this project.

## Repository layout

- `src/` — the serial packet codec, the polled Host and Node engines,   handle types, and the shared testbed shell.
- `src/transport/` — transport implementations: `mock.h` (test double),
  `serial.h` (RS-485), `serialESP32.h` (hardware TX-drain port), and
  the byte-port seam (`serialPort.h`, `serialStream.h`). The umbrella
  carries the seam (`CMRITransport.h`); a sketch includes the
  implementation it needs.
- `examples/SimpleHost` — the Host tutorial (Xiao ESP32-C6,
  OLED with segmented flush, miss/error health rows, behavior-only).
- `examples/SimpleNode` — the Node tutorial (Xiao ESP32-C6,
  pack/unpack seam, minimal GPIO).
- `examples/TracerHost` — the bench R&D tracer (Xiao ESP32-C6, USB
  CDC command stream, JSON-lines telemetry).
- `examples/TracerNode` — the bench Node test mule (capture, trace, C&C).
- `examples/XiaoNode` — the full-featured Node (OLED, WiFi OTA, I2C
  expanders via pack/unpack).
- `examples/XiaoSniffer` — a passive RS-485 bus tap (Xiao ESP32-C6,
  OLED, JSON-lines frame log).
- `tests/` — desktop unit tests (290 tests, no Arduino dependencies).
- `extras/bench/` — RS-485 bus probe scripts and single-use bench
  jigs (`XiaoBenchCal`, `XiaoBenchEcho`, `XiaoBenchEchoCancel`).
- `extras/desktop/` — the desktop tracer binary.
