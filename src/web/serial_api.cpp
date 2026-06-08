#include "serial_api.h"
#include "../core/porkchop.h"
#include "../core/config.h"
#include "../ui/display.h"
#include <string.h>
#include <ctype.h>

extern Porkchop porkchop;

static char     _buf[64];
static uint8_t  _len  = 0;
static uint8_t  _field = 0;  // 0=waiting, 1=wigle_name, 2=wigle_token, 3=wpasec

static void _processLine() {
    _buf[_len] = '\0';
    if (_len == 0) return;

    auto wifi = Config::wifi();

    if (_field == 1) {
        strncpy(wifi.wigleApiName, _buf, sizeof(wifi.wigleApiName) - 1);
        Config::setWiFi(wifi);
        Serial.printf("[SETTINGS] WiGLE name set: %s\n", wifi.wigleApiName);
        Display::showToast("WIGLE NAME SET", 2000);
        _field = 2;
        Serial.println("[SETTINGS] Now enter WiGLE API token:");
    } else if (_field == 2) {
        strncpy(wifi.wigleApiToken, _buf, sizeof(wifi.wigleApiToken) - 1);
        Config::setWiFi(wifi);
        Serial.printf("[SETTINGS] WiGLE token set.\n");
        Display::showToast("WIGLE TOKEN SET", 2000);
        _field = 0;
    } else if (_field == 3) {
        strncpy(wifi.wpaSecKey, _buf, sizeof(wifi.wpaSecKey) - 1);
        Config::setWiFi(wifi);
        Serial.printf("[SETTINGS] WPA-SEC key set.\n");
        Display::showToast("WPASEC KEY SET", 2000);
        _field = 0;
    } else {
        // Parse command
        for (uint8_t i = 0; i < _len; i++) _buf[i] = toupper(_buf[i]);

        if (strncmp(_buf, "WIGLE", 5) == 0) {
            _field = 1;
            Serial.println("[SETTINGS] Enter WiGLE account name (from wigle.net profile):");
        } else if (strncmp(_buf, "WPASEC", 6) == 0) {
            _field = 3;
            Serial.println("[SETTINGS] Enter WPA-SEC API key (from wpa-sec.stanev.org/?show_key):");
        } else if (strncmp(_buf, "CLEAR", 5) == 0) {
            memset(wifi.wigleApiName, 0, sizeof(wifi.wigleApiName));
            memset(wifi.wigleApiToken, 0, sizeof(wifi.wigleApiToken));
            memset(wifi.wpaSecKey, 0, sizeof(wifi.wpaSecKey));
            Config::setWiFi(wifi);
            Display::showToast("KEYS CLEARED", 2000);
            Serial.println("[SETTINGS] All API keys cleared.");
        } else {
            Serial.println("[SETTINGS] Commands: WIGLE | WPASEC | CLEAR");
        }
    }
    _len = 0;
}

void SerialAPI::poll() {
    if (porkchop.getMode() != PorkchopMode::SETTINGS) {
        _field = 0;
        _len = 0;
        return;
    }
    if (!Serial.available()) return;

    // Print prompt once when entering SETTINGS
    static bool promptShown = false;
    if (!promptShown) {
        Serial.println("[SETTINGS] API key entry: type WIGLE or WPASEC and press Enter");
        promptShown = true;
    }

    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            _processLine();
        } else if (_len < 62) {
            _buf[_len++] = c;
        }
    }
}
