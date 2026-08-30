// ota.h — non-blocking WiFi + ArduinoOTA lifecycle.
//
// Drops in UNCHANGED from the donor Xiao_I2C sketch. Owns all network
// semantics; knows nothing about displays or CMRInet.
//
//    OFF -> CONNECTING -> READY <-> CONNECTING   (wifi drops/reconnects)
//                         READY -> UPDATING -> reboot | error -> READY
//
//  begin() never blocks: CMRI processing starts immediately even when
//  WiFi is unavailable; OTA arms itself when the connection comes up.

#pragma once

#include <Arduino.h>
#include <IPAddress.h>

class OtaManager {
public:
    enum State : uint8_t { OFF = 0, CONNECTING, READY, UPDATING };

    typedef void (*StartHook)(void);
    typedef void (*ProgressHook)(unsigned int received, unsigned int total);
    typedef void (*EndHook)(void);
    typedef void (*ErrorHook)(const char* name);

    StartHook    onStart    = nullptr;
    ProgressHook onProgress = nullptr;
    EndHook      onEnd      = nullptr;
    ErrorHook    onError    = nullptr;

    void begin(const char* hostname, const char* ssid, const char* password);
    void poll(void);

    State     state(void) const { return _state; }
    IPAddress ip(void) const;

private:
    void arm(void);

    State       _state    = OFF;
    const char* _hostname = nullptr;
    bool        _armed    = false;
};
