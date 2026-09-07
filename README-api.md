# The CMRInet API

Part of the [CMRInet](README.md) guide set. Protocol background:
[README-protocol.md](README-protocol.md).

After setting up its environment — role, identity, provisioning — a
sketch deals with the data layer only: byte images, content freshness,
and health. The library handles packet reception and transmission,
error handling, and driving the RS-422/RS-485 bus.

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

- `CMRINodeConfig()` — the provisioning info for a Node: UA, node
  type, IN and OUT byte counts.
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
