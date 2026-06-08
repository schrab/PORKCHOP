#pragma once

#include <Arduino.h>
#include "../hal/hal_display.h"

struct PorkPatrolDetection {
    char    ssid[33];
    uint8_t bssid[6];
    int8_t  rssi;
    uint32_t firstSeen;
    uint32_t lastSeen;
    uint8_t  hitCount;
    bool     fresh;
    uint8_t  type;  // 0=FLOCK, 1=BODYCAM
};

class PorkPatrolMode {
public:
    static void start();
    static void stop();
    static void update();
    static void draw(DisplayCanvas& canvas);
    static bool isRunning();
    static uint32_t getTotalDetected();
    static uint8_t getDetectionCount();
    static uint8_t getHitType(uint8_t i);
    static int8_t  getHitRssi(uint8_t i);
    static void    getHitSSID(uint8_t i, char* buf, uint8_t len);
    static void    getHitMAC(uint8_t i, uint8_t* out);
};
