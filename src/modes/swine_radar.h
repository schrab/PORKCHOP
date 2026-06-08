#pragma once

#include <Arduino.h>
#include <esp_wifi.h>
#include "../hal/hal_display.h"

#define SWINE_RADAR_TAB_COUNT 5

// OpenDroneID contact (tab 0)
struct DroneContact {
    char    id[21];
    float   lat, lon, alt;
    float   speed;
    uint8_t mac[6];
    int8_t  rssi;
    uint32_t lastSeen;
};

// Tracker/AirTag contact (tab 1)
struct TrackerContact {
    uint8_t mac[6];
    char    vendor[16];
    int8_t  rssi;
    uint32_t firstSeen, lastSeen;
    uint16_t seenCount;
};

// Suspect AP / Stingray (tab 2)
struct SuspectAP {
    uint8_t bssid[6];
    char    reason[32];
    int8_t  rssi;
    uint32_t firstSeen, lastSeen;
    uint8_t  disappearCount;
};

// Card skimmer (tab 3)
struct SkimmerContact {
    uint8_t mac[6];
    char    name[24];
    int8_t  rssi;
    uint32_t firstSeen, lastSeen;
    uint16_t seenCount;
};

// Hostile — Pwnagotchi / Flipper / Pineapple (tab 4)
struct HostileContact {
    uint8_t mac[6];
    char    type[16];
    char    detail[48];
    int8_t  rssi;
    uint32_t firstSeen, lastSeen;
    uint16_t seenCount;
};

class SwineRadarMode {
public:
    static void start();
    static void stop();
    static void update();
    static void draw(DisplayCanvas& canvas);
    static bool isRunning();

    // Tab control
    static uint8_t getTab();
    static void    setTab(uint8_t t);

    // Tab data accessors
    static uint8_t getDroneCount();
    static uint8_t getTagCount();
    static uint8_t getSuspectCount();
    static uint8_t getSkimmerCount();
    static uint8_t getHostileCount();

    static void getDroneInfo(uint8_t i, char* id, float* lat, float* lon, float* alt, float* spd);
    static void getTagInfo(uint8_t i, uint8_t* mac, char* vendor, int8_t* rssi);
    static void getSuspectInfo(uint8_t i, uint8_t* bssid, char* reason, int8_t* rssi);
    static void getSkimmerInfo(uint8_t i, uint8_t* mac, char* name, int8_t* rssi);
    static void getHostileInfo(uint8_t i, uint8_t* mac, char* type, char* detail, int8_t* rssi);
};
