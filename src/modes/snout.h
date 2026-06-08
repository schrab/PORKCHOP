#pragma once

#include <Arduino.h>
#include "../hal/hal_display.h"

namespace SnoutMode {

struct EvilTwin {
    char    ssid[33];
    uint8_t bssid1[6];
    uint8_t bssid2[6];
    uint8_t auth1, auth2;
    int8_t  rssi1, rssi2;
    uint32_t seenAt;
};

struct HiddenResult {
    uint8_t bssid[6];
    char    ssid[33];
    uint8_t channel;
    bool    revealed;
};

void start();
void stop();
void update();
void draw(DisplayCanvas& canvas);

bool isRunning();
uint8_t getTab();
void    setTab(uint8_t t);
uint8_t getTwinCount();
void    getTwin(uint8_t i, char* ssid, uint8_t* b1, uint8_t* b2, uint8_t* a1, uint8_t* a2);
bool    isStormActive();
uint16_t getDeauthPeak();
uint16_t getDeauthRate();
void    getStormSrc(uint8_t* out);
uint8_t getHiddenCount();
void    getHiddenEntry(uint8_t i, uint8_t* bssid, char* ssid, bool* revealed);

} // namespace SnoutMode
