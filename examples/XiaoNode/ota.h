// ota.h — non-blocking WiFi + ArduinoOTA lifecycle.
//
// Owns all network semantics; knows nothing about displays or CMRInet.
//
//    OFF -> CONNECTING -> READY     (got an IP)
//                      -> FAILED    (initial connect timed out; stop)
//         READY <-> CONNECTING      (link drop after a successful join;
//                                    auto-reconnect only after first READY)
//         READY -> UPDATING -> reboot | error -> READY
//
//  begin() never blocks: CMRI processing starts immediately even when
//  WiFi is unavailable; OTA arms itself when the connection comes up.
//  A first-connect timeout (default 30 s) stops spinning forever on
//  bad credentials — FAILED is terminal until the board reboots.

#pragma once

#include <Arduino.h>
#include <IPAddress.h>

class OtaManager {
public:
    enum State : uint8_t {
        OFF = 0,
        CONNECTING,
        READY,
        UPDATING,
        FAILED,  // initial join never completed; not retrying
    };

    typedef void (*StartHook)(void);
    typedef void (*ProgressHook)(unsigned int received, unsigned int total);
    typedef void (*EndHook)(void);
    typedef void (*ErrorHook)(const char* name);

    StartHook    onStart    = nullptr;
    ProgressHook onProgress = nullptr;
    EndHook      onEnd      = nullptr;
    ErrorHook    onError    = nullptr;

    /// Start STA join. `connectTimeoutMs` bounds the first attempt;
    /// 0 means never give up (legacy donor behavior).
    void begin(const char* hostname, const char* ssid, const char* password,
               uint32_t connectTimeoutMs = 30000);
    void poll(void);

    State     state(void) const { return _state; }
    IPAddress ip(void) const;

private:
    void arm(void);
    void failJoin_(const char* reason);

    State       _state    = OFF;
    const char* _hostname = nullptr;
    bool        _armed    = false;
    bool        _everReady = false;  // true after first successful join
    uint32_t    _connectStartedMs = 0;
    uint32_t    _connectTimeoutMs = 30000;
};
