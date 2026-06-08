#include "ghost.h"
#include "config.h"
#include "sdlog.h"
#include <esp_wifi.h>

static uint8_t sCurrentMAC[6] = {};
static uint8_t sRealMAC[6]    = {};
static uint32_t sLastRotate   = 0;
static bool     sRealSaved    = false;

static void randomMAC(uint8_t* mac) {
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    mac[0] = ((uint8_t)(r1 >> 0) & 0xFE) | 0x02;
    mac[1] = (uint8_t)(r1 >>  8);
    mac[2] = (uint8_t)(r1 >> 16);
    mac[3] = (uint8_t)(r2 >>  0);
    mac[4] = (uint8_t)(r2 >>  8);
    mac[5] = (uint8_t)(r2 >> 16);
}

static void setMAC(const uint8_t* mac) {
    esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, mac);
    if (err == ESP_OK) {
        memcpy(sCurrentMAC, mac, 6);
        Serial.printf("[GHOST] MAC -> %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        Serial.printf("[GHOST] set_mac failed: %d\n", (int)err);
    }
}

void GhostMode::init() {
    sLastRotate = 0;
    sRealSaved = false;
    memset(sCurrentMAC, 0, 6);
    memset(sRealMAC, 0, 6);
}

void GhostMode::apply() {
    if (!sRealSaved) {
        esp_wifi_get_mac(WIFI_IF_STA, sRealMAC);
        sRealSaved = true;
    }
    if (Config::wifi().ghostEnabled) {
        uint8_t mac[6]; randomMAC(mac); setMAC(mac);
        sLastRotate = millis();
        SDLog::log("GHOST", "MAC rotated");
    } else {
        setMAC(sRealMAC);
        sLastRotate = 0;
        SDLog::log("GHOST", "MAC restored to real");
    }
}

void GhostMode::update() {
    if (!Config::wifi().ghostEnabled) return;
    if (sLastRotate == 0) { sLastRotate = millis(); return; }
    uint32_t interval = (uint32_t)Config::wifi().ghostInterval * 60000UL;
    if (millis() - sLastRotate < interval) return;
    uint8_t mac[6]; randomMAC(mac); setMAC(mac);
    sLastRotate = millis();
    SDLog::log("GHOST", "Auto-rotated (%umin interval)", Config::wifi().ghostInterval);
}

bool GhostMode::isActive() {
    return Config::wifi().ghostEnabled;
}

void GhostMode::getMAC(uint8_t* out) {
    memcpy(out, sCurrentMAC, 6);
}

void GhostMode::realMAC(uint8_t* out) {
    if (sRealSaved) {
        memcpy(out, sRealMAC, 6);
    } else {
        esp_wifi_get_mac(WIFI_IF_STA, out);
    }
}
