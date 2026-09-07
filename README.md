# CMRInet
| [Installation](README-install.md) | [Hardware Details](README-hardware.md)  | [Tutorials](README-tutorials.md)  | [API Documentation](README-api.md) |  [CMRInet Protocol Details](README-protocol.md)  | [References](README-references.md) |

CMRInet is an Arduino library for the NMRA CMRInet protocol (LCS-9.10.1): layout I/O over a serial bus. A Host transmits outputs to Nodes and polls them for inputs; each Node reports its inputs and applies the outputs it receives. This library implements both roles — Host and Node engines over a shared protocol codec and pluggable transports. The Node software works with JMRI as the Host.


In the repo: 
- the `CMRIHost` and `CMRINode` engines
- tutorial sketches `SimpleNode`, `SimpleHost`
- a full-featured `XiaoNode` (I2C expanders, OLED, WiFi OTA)
- bench tooling (`TracerHost`, `TracerNode`, `XiaoSniffer`), and
- a desktop Host.

CMRInet implements a three layer architecture: 
1. data (INPUT/OUTPUT byte images), 
2. protocol (I/T/P/R packets), and
3. physical (framing, wiring, timing). 

The Host and Node sketches define the CMRI network configuration (node addresses, types...) and manage the data layer; the CMRInet library does the rest.

- CMRI Bus: 28800 baud, 8N2, RS-422 (4-wire) or RS-485 (2-wire). Host T± → Node R±, Host R± → Node T±; Nodes daisy-chain straight through, plus to plus, minus to minus.
- Status: v0.1.0, MIT license. The Host and Node engines are complete; desktop unit tests cover the codec, both engines, and the transports; the example sketches run on cpNode-Xiao hardware.



## Installation

Clone the CMRInet library into your Arduino libraries folder, install the Arduino IDE, the ESP32/Xiao platform files and the Adafruit SSD1306 and GFX libraries.  Full details are in the [Installation Guide](README-install.md).

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

  node.config({.ua          = 30,   // the Host and Node interact using this address
               .nodeType    = 'C',  // JMRI's 'C type' node
               .inputBytes  = 2,    // 2 bytes for onboard I/O
               .outputBytes = 2});
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

## License
[MIT License](LICENSE).
