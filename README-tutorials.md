# [CMRInet](README.md)
| [Installation](README-install.md) | [Hardware Details](README-hardware.md)  | **Tutorials**  | [API Documentation](README-api.md) |  [CMRInet Protocol Details](README-protocol.md)  | [References](README-references.md) |

# Tutorials

The `examples/` folder holds useful sketches; they appear under Arduino IDE's **File > Examples > CMRInet** menu:

- `SimpleNode` — the front-door tutorial: the onboard LED and pushbutton, with JMRI as the Host.
- `SimpleHost` — polls a table of remote Nodes, shows each Node's health on an OLED, and runs demo behavior services.
- `XiaoNode` — the production Node: up to 8 MCP23017 I2C expanders, OLED I/O grid, WiFi OTA firmware updates.
- `TracerHost` and `TracerNode` — the instrumented bench pair used by this repo's validation tooling.
- `XiaoSniffer` — RS-422/RS-485 bus sniffer / data logger.

## Installation

Clone the CMRInet library into your Arduino libraries folder, install the Arduino IDE, the ESP32/Xiao platform files and the Adafruit SSD1306 and GFX libraries.  Full details are in the [Installation Guide](README-install.md).

## Run SimpleNode
Requirements: One cpNode-Xiao board plus a JMRI Host on your
computer is enough.

1. Open `examples/SimpleNode/SimpleNode.ino`.
2. Set the Unit Address. It must match the address that the Host
   polls.
```
  node.config({.ua          = 30,   // must match the Host's polled address
               .nodeType    = 'C',  // JMRI's 'C type' node
               .inputBytes  = 2,    // 2 bytes for onboard I/O
               .outputBytes = 2});
```
3. Edit the two callbacks. `packInputs()` runs when the Host polls
   this Node. `unpackOutputs()` runs when the Node receives a
   TRANSMIT.
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

You should see this. JMRI polls the Node as a "C" type node at UA 30.
The onboard LED follows turnout `CMRI:ct0001`. The pushbutton changes
sensor `CMRI:cs0002`.

## Run SimpleHost

SimpleHost needs a Node on the bus. A Node that is not connected
reports `OFFLINE`. The Host keeps polling it, and the rotation does
not stall.

1. Open `examples/SimpleHost/SimpleHost.ino`.
2. Edit `nodeTable`. Write one `HostNodeSpec` row for each remote
   Node. `CpnodeInit()` takes the TOTAL image size in each direction:
   the 2 onboard bytes that a "C" type Node reserves, plus the
   expander bytes. Each expander contributes one input byte and one
   output byte, so IN + OUT - 4 = 2 x the number of expanders in use.
```
HostNodeSpec nodeTable[] = {
  // UA 30: CPNODE — 2 onboard + 5 IOX bytes in and out
  hostNodeCpnode(30, CpnodeInit(2 + 5, 2 + 5 /* o1=0, o2=0 */)),
  // UA 31: CPNODE — 2 onboard + 1 IOX byte in and out
  hostNodeCpnode(31, CpnodeInit(2 + 1, 2 + 1 /* o1=0, o2=0 */)),
  // Examples of other types (uncomment / edit as needed):
  hostNodeSmini(5, CMRInet::SminiInit(/*ns=*/0)),
  // hostNodeSusic(10, CMRInet::UsicFamilyInit(/*ns=*/1, /*NI=*/4, /*NO=*/4)),
};
```
3. Upload. Open the serial monitor at 115200 baud.

You should see this. The OLED shows one row per Node with its health
state, and the miss and error counts. The behavior services walk bits
across the UA 30 and UA 31 outputs, and copy one UA 31 input to a
UA 30 output. When a reply does not match what the Host expects, the
serial monitor prints a `REJECT:` line with the reason.

## Run XiaoNode

XiaoNode drives real layout I/O through I2C expanders.

1. Open `examples/XiaoNode/XiaoNode.ino`.
2. Set `NODE_ID` to the address that the Host polls.
3. Edit the `expanders[]` table to match your I2C bus. Each row gives
   one MCP23017 address and the direction of its two ports. The
   callbacks read this table, so you do not edit them.
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
4. If you want WiFi OTA, copy `secrets.h.example` to `secrets.h` and
   set your credentials. Comment out `USE_OTA` to build without WiFi.
   Comment out `USE_OLED` to build without the display.
5. Upload.

You should see this. The OLED shows a bit grid, one row per expander,
and it halo-boxes recent changes. The bottom line shows the WiFi and
OTA state. The sketch answers OTA at its mDNS name, `XiaoC6-30` by
default.

## Troubleshooting

- No traffic at all. Check the baud rate on both ends. Check that the
  Host's T± pair reaches the Node's R± pair.
- A `REJECT:` line with a geometry reason. The byte counts in the
  Host's `nodeTable` do not match the Node's `cfg.inputBytes` and
  `cfg.outputBytes`.
- A `REJECT:` line with a UA reason. The UA in the Host's `nodeTable`
  does not match the Node's `cfg.ua`.
- The OLED stays blank. Check that `USE_OLED` is defined. Check that
  the display sits at I2C address 0x3C, with SDA on D4 and SCL on D5.
