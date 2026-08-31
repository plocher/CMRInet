// ota.cpp — non-blocking WiFi + ArduinoOTA lifecycle.

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

void OtaManager::begin(const char* hostname, const char* ssid,
                       const char* password, uint32_t connectTimeoutMs) {
    _hostname = hostname;
    _connectTimeoutMs = connectTimeoutMs;
    _connectStartedMs = millis();
    _everReady = false;
    _armed = false;

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);   // modem power-save stalls OTA TCP transfers on C6
    // Auto-reconnect is useful after a real join; leave it off until then
    // so a bad SSID/password does not thrash the radio forever.
    WiFi.setAutoReconnect(false);
    WiFi.begin(ssid, password);

    _state = CONNECTING;
}

void OtaManager::failJoin_(const char* reason) {
    WiFi.disconnect(true /* wifioff */, false /* eraseAP */);
    WiFi.setAutoReconnect(false);
    _state = FAILED;
    // Brief full-screen notice via the OTA error hook is intentional;
    // after HOLD the live status line shows NET_FAILED ("WiFi: ...").
    if (onError) {
        onError(reason);
    }
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
        // Transfer failed but the link is still up; back to READY so the
        // next handle() cycle can accept another attempt.
        if (_state == UPDATING) {
            _state = READY;
        }
    });

    ArduinoOTA.begin();
    _armed = true;
}

void OtaManager::poll(void) {
    if (_state == OFF || _state == FAILED) {
        return;
    }

    // Do not clobber an in-progress OTA with READY/CONNECTING.
    if (_state == UPDATING) {
        if (_armed) {
            ArduinoOTA.handle();
        }
        return;
    }

    const bool connected = (WiFi.status() == WL_CONNECTED);
    if (connected) {
        if (!_everReady) {
            _everReady = true;
            // Now that credentials worked once, allow drop recovery.
            WiFi.setAutoReconnect(true);
        }
        if (!_armed) {
            arm();
        }
        _state = READY;
        if (_armed) {
            ArduinoOTA.handle();
        }
        return;
    }

    // Not connected.
    if (_everReady) {
        // Had a real join before: wait for auto-reconnect.
        _state = CONNECTING;
        return;
    }

    // Still on the first join attempt.
    _state = CONNECTING;
    if (_connectTimeoutMs != 0 &&
        (millis() - _connectStartedMs) >= _connectTimeoutMs) {
failJoin_("WiFi: can't connect");
    }
}

IPAddress OtaManager::ip(void) const {
    return WiFi.localIP();
}
