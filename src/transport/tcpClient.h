// tcpClient.h — Arduino adapter: TcpPort over an Arduino Client
// (WiFiClient, EthernetClient, etc.).
//
// Arduino-only: this header compiles under the Arduino toolchain and
// is excluded from desktop builds, which exercise the transport
// through a fake port instead. Nothing here allocates.
//
// VALIDATION: Design v1.7 D1: Transport implementations end with the
// interface they implement (ClientTcpPort over TcpPort).
// VALIDATION: Design v1.1 D6: every method must be non-blocking.
// VALIDATION: ADR-0004: TCP as the alternate carrier for JMRI interop.

#pragma once

#ifdef ARDUINO

#include <Arduino.h>
#include <Client.h>

#include "tcpPort.h"

namespace CMRInet {

/// Adapter that wraps any Arduino Client (e.g. WiFiClient, EthernetClient)
/// as a CMRInet TcpPort.
class ClientTcpPort : public TcpPort {
 public:
  /// `client`: the configured and managed Arduino network client.
  /// The client must outlive this port.
  explicit ClientTcpPort(Client& client) : client_(client) {}

  void begin() override {}

  bool connected() const override {
    return client_.connected();
  }

  int readByte() override {
    if (!client_.connected() && client_.available() == 0) {
      return -1;
    }
    return client_.read();
  }

  size_t writeBytes(const uint8_t* bytes, size_t length) override {
    if (!client_.connected() || length == 0) {
      return 0;
    }
    return client_.write(bytes, length);
  }

  bool transmitDrained() const override {
    return true;
  }

 private:
  Client& client_;
};

}  // namespace CMRInet

#endif  // ARDUINO
