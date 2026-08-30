// ota.cpp — non-blocking WiFi + ArduinoOTA lifecycle.
// Drops in UNCHANGED from the donor Xiao_I2C sketch.

#include "ota.h"

#include <WiFi.h>
#include <ArduinoOTA.h>

static const char* errorName(ota_error_t err) {
    switch (err) {
        case OTA_AUTH_ERROR:    return "Auth Failed";
        case OTA_BEGIN_ERROR:   return "Begin Failed";
        case OTA_CONNECT_ERROR: return "Connect Failed";
        case OTA_RECEIVE_ERROR: return "Receive Failed";
        case OTA_END_ERROR:     return "End Failed";
        default:                return "Unknown Error";
    }
}

void OtaManager::begin(const char* hostname, const char* ssid, const char* password) {
    _hostname = hostname;

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);   // modem power-save stalls OTA TCP transfers on C6
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid, password);

    _state = CONNECTING;
}

void OtaManager::arm(void) {
    ArduinoOTA.setHostname(_hostname);

    ArduinoOTA.onStart([this]() {
        _state = UPDATING;
        if (onStart) onStart();
    });
    ArduinoOTA.onProgress([this](unsigned int received, unsigned int total) {
        if (onProgress) onProgress(received, total);
    });
    ArduinoOTA.onEnd([this]() {
        if (onEnd) onEnd();
    });
    ArduinoOTA.onError([this](ota_error_t err) {
        if (onError) onError(errorName(err));
    });

    ArduinoOTA.begin();
    _armed = true;
}

void OtaManager::poll(void) {
    if (_state == OFF) return;

    bool connected = (WiFi.status() == WL_CONNECTED);
    if (connected && !_armed) {
        arm();
    }
    _state = connected ? READY : CONNECTING;

    if (_armed && connected) {
        ArduinoOTA.handle();
    }
}

IPAddress OtaManager::ip(void) const {
    return WiFi.localIP();
}
