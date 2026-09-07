# CMRInet

CMRInet is the NMRA standard (LCS-9.10.1) for moving I/O over a serial bus.  In CMRInet, a Host transmits to, and polls Nodes; each Node reports its inputs and applies the outputs it receives.  This library implements the standard for both roles.

This repository provides
 - **Host and Node engines** for Arduino sketches, using a shared protocol codec and pluggable transports.
 - **Tutorial sketches**:
   - `SimpleNode` works on a standalone cpNode-Xiao board, using the built in LED and an onboard pushbutton. 
   - `SimpleHost` illustrates remote Node identification and configuration, with service behaviors that demonstrate output animation and logic that reads inputs and writes outputs. The sketch polls a predefined list of remote nodes, shows each node's health on an OLED, and runs a set of simplistic behavior services: blinking outputs and an output that follows an input.
  
 - **Full featured Node sketch**: 
   - `XiaoNode` is a production ready example that supports up to 8x cpNode-IOX, MRCS IOX-16 or (4x) IOX-32 I2C expander boards, an onboard OLED display and WiFi OTA for firmware updates. 
  
 - **Benchtop tooling**: 
   - `TracerHost` and `TracerNode` work with the validation tooling in this repo to provide an instrumented remote controlled test suite.
   - `XiaoSniffer` RS-422/RS-485 bus sniffer / data logger
   - **A desktop Host implementation**: `extras/desktop/` that runs on your computer using an USB to RS422/RS485 dongle.  This application implements a CMRIHost over SerialCMRITransport over PosixSerialPort, driven by the same src/testbed/TracerShell.h C&C engine as TracerHost.

The Node software works with JMRI as a Host.

## Architecture at a glance

This library implements the NMRA CMRInet specification's three layers:
1. the data layer - arrays of INPUT and OUTPUT bytes.
2. the serial protocol layer - I, T, P and R packets passed between Hosts and Nodes
3. the physical layer - the framing and makeup of the packets, wiring, bit representations and timing.

After setting up its environment (Host or Node, identity, provisioning...), a sketch deals with the data layer, content freshness and health, leaving the library to handle the mechanics of packet reception and transmission, error handling, and driving the RS-422/RS485 bus.

### Host API
  - `addRemoteNode()` - register one node.
  - `host.node(UA)` - return the `RemoteNodeHandle` for a provisioned Node.
  - `handle->setOutputBit(byte, bit, value)` and `handle->inputBit(byte, bit)` - write and read data associated with a Node
  - `host.begin()` - initializes and configures the protocol engine, called in setup()
  - `host.tick(millis())` - runs the non blocking CMRI protocol engine, called in loop()

  - `RemoteNodeHandle`s,expose input age, health state (`RemoteNodeState`), and statistics.

### Node API
  - `CMRINodeConfig()` - provision info for a Node: UA, node type, IN and OUT byte counts
  - `node.onPack(callback)` - called when Node is POLLed, use `ib.setBit(byte, bit, value)` to record input state.
  - `node.onUnpack(callback)` - called when the node receives a TRANSMIT, use `ob.getBit(byte, bit)` to get the desired output state.
  - `node.begin()` - initializes and configures the protocol engine, called in setup()
  - `node.tick(millis())` - runs the non blocking CMRI protocol engine, called in loop()

## Installation
You will need:

- Arduino IDE 1.6.2 or later
- A cpNode-Xiao board or equivalent.
- For Nodes, 23017-based 16-bit I2C expanders such as the cpNode-IOX, MRCS' IOX-16 and -32, generic breakout boards, etc.  Up to 8x expander devices are supported on a single Node.
- For the OLED examples: the  `Adafruit SSD1306` and `Adafruit GFX` libraries.  To compile without the display, comment out `USE_OLED` at the top of the sketch.
- For WiFi-based over the air firmware updates: the `ArduinoOTA` library.  To compile without WiFi OTA, comment out `USE_OTA` at the top of the sketch.

1. Install the Espressif board package.
   - File > Preferences: add `https://espressif.github.io/arduino-esp32/package_esp32_index.json` to the list of additional board manager URLs.
   - Tools > Board > Boards Manager: install the `esp32` package.
   - Select the "Seeed Studio XIAO ESP32C6" board.
2. Install the display libraries if your sketch uses the OLED.
   - Libraries manager: 
     - `Adafruit SSD1306` and `Adafruit GFX`.
   - `ArduinoOTA` ships with the ESP32 core. You do not install it.
3. Get the CMRInet library.
```
Linux/MacOS
   git clone https://github.com/plocher/CMRInet ~/Documents/Arduino/libraries/CMRInet

Windows 
   git clone https://github.com/plocher/CMRInet Documents\Arduino\libraries\CMRInet
```
4. Restart the Arduino IDE so that the newly installed libraries and CMRInet examples appear under File > Examples > CMRInet.

### Serial settings

The examples transmit at 28800 baud, 8 data bits, no parity, two stop bits.
If your Host is JMRI, match both ends before you expect traffic. JMRI's
CMRI default is 19200 baud, but can easily be changed.

## Hardware
### cpNode-Xiao
 - Board details [on SPCoast.com](https://www.spcoast.com/versions/cpnode-xiao/)
 - Design files [on github.com](https://github.com/plocher/cpNode-Xiao)
 - The cpNode-Xiao is a SPCoast/MRCS product that combines a Seeed Xiao module with a CMRI RS422 4-wire bus interface, an OLED display and a 5 V I2C bus that connects to I2C IO expanders.
 - Pinout
```
   D7 - GPIO17  RX      CMRI 4-wire RS485 receive
   D6 - GPIO16  TX      CMRI 4-wire RS485 transmit
   D5 - GPIO23  SCL     I2C
   D4 - GPIO22  SDA     I2C
   D3 - GPIO21  TXEN    RS422/485 transmit enable
   D2 - GPIO02          cpNode-Xiao onboard pushbutton input (active-low)
   na - GPIO15  LED_BUILTIN (not brought out to a XIAO pin)
```
### cpNode-IOX
  - Board details: [on SPCoast.com](https://www.spcoast.com/versions/cpnode-iox/)
  - Design files: [on github.com](https://github.com/plocher/cpNode-IOX)
  - The cpNode-IOX is a follow-on to MRCS' IOX series of expanders. It provides:
    - local 5 V, 750 mA regulated power for attached accessories
    - 0.100 inch pitch screw terminals for layout connections
    - I2C address selection and display
    - Daisy-chained 7.5 to 12 V DC unregulated power source feed through

### Bus Wiring:
 - 4-wire RS422 topology
   - Host's T± to the Node's R± and 
   - Host's R± to the Node's T±. This is a crossover cable.
   - All the Nodes on the bus are wired with a straight through cable, their T± pairs and R± pairs daisy chained to each other, plus to plus, minus to minus.
 - 2-wire RS485 topology
   - All the devices (Host and Node) are wired straight thru with their A+/B- pair daisy chained to each other, plus to plus, minus to minus.
 - Hybrid 4-wire in a 2-wire topology
   - Many boards in the CMRI ecosystem have a 5-pin bus connector, with a T± pair, an R± pair and Shield.
   - You can use them with a 2-wire device by
     - connecting all the 4-wire devices as "4-wire" above.
     - looping back the T± pair to the R± pair at one end of the 4-wire segment
     - connecting the A+/B- RS485 pair to either the T± pair OR the R± pair (since the previous step connected them, there is no difference between them anymore)
     - This ONLY WORKS WITH CMRI Host implementations and RS485 interface hardware that supports TX Enable.  CMRInet and the cpNode-Xiao fully support TXEN in a full duplex RS485 environment.

## Run an example

Start with the Node. One cpNode-Xiao board plus a JMRI Host on your computer is enough.

### Run SimpleNode

1. Open `examples/SimpleNode/SimpleNode.ino`.
2. Set the Unit Address. It must match the address that the Host polls.
```
  cfg.ua          = 30;
```
3. Edit the two callbacks. `packInputs()` runs when the Host polls this Node. `unpackOutputs()` runs when the Node receives a TRANSMIT.
```
// SimpleNode uses two of the onboard bits that a "C" type node reserves.
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
4. Select the board and the port. Upload.

You should see this. JMRI polls the Node as a "C" type node at UA 30. The onboard LED follows turnout `CMRI:ct0001`. The pushbutton changes sensor `CMRI:cs0002`.

### Run SimpleHost

SimpleHost needs a Node on the bus. A Node that is not connected reports `OFFLINE`. The Host keeps polling it, and the rotation does not stall.

1. Open `examples/SimpleHost/SimpleHost.ino`.
2. Edit `nodeTable`. Write one `HostNodeSpec` row for each remote Node.
```
// Each expander is 16 bits in two 8-bit ports. One port is all input or all
// output, so one expander contributes 2 bytes to the images. CpnodeInit()
// takes the TOTAL image size in each direction: the 2 onboard bytes that a
// "C" type node reserves, plus the expander bytes. Therefore
//     IN + OUT - 4 == 2 x the number of expanders in use.
HostNodeSpec nodeTable[] = {
  // UA 30: CPNODE — 2 onboard + 0 expander bytes in and out
  hostNodeCpnode(30, CpnodeInit(2, 2)),
  // Examples of other types
  hostNodeCpnode(31, CpnodeInit(10, 10)), // 8 expanders, 8 bytes in and 8 out
  hostNodeSmini(5, CMRInet::SminiInit(/*ns=*/0)),
  hostNodeSusic(10, CMRInet::UsicFamilyInit(/*ns=*/1, /*NI=*/4, /*NO=*/4)),
}
```
3. Upload. Open the serial monitor at 115200 baud.

You should see this. The OLED shows one row per Node with its health state, and the miss and error counts. The behavior services walk bits across the UA 30 and UA 31 outputs, and copy one UA 31 input to a UA 30 output. When a reply does not match what the Host expects, the serial monitor prints a `REJECT:` line with the reason.

### Run XiaoNode

XiaoNode drives real layout I/O through I2C expanders.

1. Open `examples/XiaoNode/XiaoNode.ino`.
2. Set `NODE_ID` to the address that the Host polls.
3. Edit the `expanders[]` table to match your I2C bus. Each row gives one MCP23017 address and the direction of its two ports. The callbacks read this table, so you do not edit them.
```
IOX_Config expanders[] =
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
4. If you want WiFi OTA, copy `secrets.h.example` to `secrets.h` and set your credentials. Comment out `USE_OTA` to build without WiFi. Comment out `USE_OLED` to build without the display.
5. Upload.

You should see this. The OLED shows a bit grid, one row per expander, and it halo-boxes recent changes. The bottom line shows the WiFi and OTA state. The sketch answers OTA at its mDNS name, `XiaoC6-30` by default.

## Troubleshooting

- No traffic at all. Check the baud rate on both ends. Check that the Host's T± pair reaches the Node's R± pair.
- A `REJECT:` line with a geometry reason. The byte counts in the Host's `nodeTable` do not match the Node's `cfg.inputBytes` and `cfg.outputBytes`.
- A `REJECT:` line with a UA reason. The UA in the Host's `nodeTable` does not match the Node's `cfg.ua`.
- The OLED stays blank. Check that `USE_OLED` is defined. Check that the display sits at I2C address 0x3C, with SDA on D4 and SCL on D5.

## Documents

In this repository:

- [docs/lcs-9.10.1_cmrinet_v1.1.pdf](docs/lcs-9.10.1_cmrinet_v1.1.pdf) — the NMRA specification.
- [docs/cmrinet-interop-profile-and-errata.md](docs/cmrinet-interop-profile-and-errata.md) — "CMRInet as fielded". It holds the normative interop rules and the proposed LCS-9.10.1 errata, each cited to its evidence.
- [docs/DESIGN.md](docs/DESIGN.md) — the architecture of this library, and decisions D1 through D17.
- [docs/research/](docs/research/) — seven adversarial reviews of fielded implementations against the spec, four Node-side and three Host-side, plus the synthesis in [comparison.md](docs/research/comparison.md).
- [docs/CHANGELOG.md](docs/CHANGELOG.md) — what changed in each release.

Outside this repository:

- Bruce Chubb's *C/MRI User's Manual* and the two volumes of the *Railroaders Handbook* are the standard field references for CMRI practice. Get them from [JLC Enterprises](https://www.jlcenterprises.net/pages/downloads).
- [JMRI documents its CMRI Host support](https://www.jmri.org/help/en/html/hardware/cmri/CMRI.shtml) in the JMRI help pages.

## Repository layout

- `src/` — the packet codec, the Host and Node engines, the handle types, and the shared testbed shell.
- `src/transport/` — the transports. `mock.h` is the test double. `serial.h` drives the bus. `serialESP32.h` adds the ESP32 hardware transmit drain. `serialPort.h` and `serialStream.h` are the byte-port seam, and `CMRITransport.h` carries it. A sketch includes the transport it needs.
- `examples/` — the six sketches described above.
- `tests/` — desktop unit tests. They do not depend on Arduino.
- `extras/bench/` — bus probe scripts and the single-use bench jigs. See its own README.
- `extras/desktop/` — the desktop Host tracer.

## Developers

`make check` runs all three compile-time gates: the desktop unit tests, the desktop tracer build, and the example-sketch warning gate. [docs/sketch-warning-gate.md](docs/sketch-warning-gate.md) explains why the sketch gate runs apart from the build.

Work on a feature branch and open a pull request. Commit messages follow the conventional-commit style that the history uses.

## License and status

MIT. See [LICENSE](LICENSE).

The Host and Node engines are complete. The desktop unit tests cover the codec, both engines, and the transports. The example sketches run on cpNode-Xiao hardware.

