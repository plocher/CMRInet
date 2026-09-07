# [CMRInet](README.md)
| [Installation](README-install.md) | [Hardware Details](README-hardware.md)  | [Tutorials](README-tutorials.md)  | **API Documentation** |  [CMRInet Protocol Details](README-protocol.md)  | [References](README-references.md) |

# The CMRInet API

After setting up its environment — role, identity, provisioning — a
sketch deals with the data layer only: byte images, content freshness,
and health. The CMRInet library handles packet reception and transmission,
error handling, and driving the RS-422/RS-485 bus.

## Repository layout

- `src/` — the CMRI packet codec, the Host and Node engines, the handle
  types, and the shared testbed shell.
- `src/transport/` — the transports. 
  - `mock.h` is the test double. 
  - `serial.h` drives the bus.
     - `serialESP32.h` adds the ESP32 hardware transmit drain. 
     - `serialPort.h` and `serialStream.h` are the byte-port interface.
  - `CMRITransport.h` carries it. A sketch includes the transport it needs.
- `examples/` — tutorials, test tooling and production use of the CMRInet library
- `tests/` — desktop unit tests. 
- `extras/bench/` — bus probe scripts and the single-use bench jigs. See the [Bench README](./extras/bench/README.md).
- `extras/desktop/` — the desktop Host tracer: a CMRIHost over a POSIX serial port, driven by the same testbed shell as TracerHost.

## Developers

`make check` runs all three compile-time gates: the desktop unit
tests, the desktop tracer build, and the example-sketch warning gate.
[docs/sketch-warning-gate.md](docs/sketch-warning-gate.md) explains
why the sketch gate runs apart from the build.

Work on a feature branch and open a pull request. Commit messages
follow the conventional-commit style that the history uses.


## Host API

- `addRemoteNode()` — register one Node.
- `host.node(UA)` — return the `RemoteNodeHandle` for a provisioned
  Node.
- `handle->setOutputBit(byte, bit, value)` and
  `handle->inputBit(byte, bit)` — write and read the data associated
  with a Node.
- `host.begin()` — initialize and configure the protocol engine. Call
  it in `setup()`.
- `host.tick(millis())` — run the non-blocking CMRI protocol engine.
  Call it in `loop()`.

`RemoteNodeHandle`s expose input age, health state
(`RemoteNodeState`), and statistics.

## Node API

- `CMRINodeConfig` — the provisioning info for a Node: UA, node type,
  IN and OUT byte counts.
- `node.config({...})` — set the provisioning before `begin()`, e.g.
  `node.config({.ua = 30, .nodeType = 'C', .inputBytes = 2, .outputBytes = 2});`
- `node.onPack(callback)` — called when the Node is POLLed. Use
  `ib.setBit(byte, bit, value)` to record input state.
- `node.onUnpack(callback)` — called when the Node receives a
  TRANSMIT. Use `ob.getBit(byte, bit)` to get the desired output
  state.
- `node.begin()` — initialize and configure the protocol engine. Call
  it in `setup()`.
- `node.tick(millis())` — run the non-blocking CMRI protocol engine.
  Call it in `loop()`.

Worked usage: [README-tutorials.md](README-tutorials.md).
