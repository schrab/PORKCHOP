// GPS AT6668 Module Interface
#pragma once

#include <Arduino.h>
#include <TinyGPSPlus.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct GPSData {
    double latitude;
    double longitude;
    double altitude;
    float speed;
    float course;
    uint8_t satellites;
    uint16_t hdop;
    uint32_t date;
    uint32_t time;
    bool valid;
    bool fix;
    uint32_t age;  // Age of last fix in ms
};

// NMEA ring buffer for diagnostic display
#define NMEA_RING_LINES 8
#define NMEA_LINE_MAX 96

class GPS {
public:
    static void init(uint8_t rxPin, uint8_t txPin, uint32_t baud = 9600);
    static void reinit(uint8_t rxPin, uint8_t txPin, uint32_t baud);  // Re-init with new pins
    static void update();
    static void sleep();
    static void wake();
    static void ensureContinuousMode();  // Force continuous mode regardless of software state
    
    static bool hasFix();
    static GPSData getData();
    static void getTimeString(char* out, size_t len);
    static bool getLocationString(char* out, size_t len);
    
    // Power management
    static void setPowerMode(bool active);
    static bool isActive();
    
    // Statistics
    static uint32_t getFixCount();
    static uint32_t getLastFixTime();
    
    // NMEA diagnostic accessors
    static uint8_t getNmeaLineCount();
    static const char* getNmeaLine(uint8_t index);  // 0=oldest, count-1=newest
    static uint32_t getTotalBytesProcessed();
    
private:
    static TinyGPSPlus gps;
    static HardwareSerial* serial;
    static bool active;
    static GPSData currentData;
    static uint32_t fixCount;
    static uint32_t lastFixTime;
    static uint32_t lastUpdateTime;
    static SemaphoreHandle_t mutex;
    
    // NMEA ring buffer
    static char nmeaRing[NMEA_RING_LINES][NMEA_LINE_MAX];
    static uint8_t nmeaHead;  // next write position
    static uint8_t nmeaCount; // number of valid lines (0..NMEA_RING_LINES)
    static char nmeaLineBuf[NMEA_LINE_MAX];  // partial line accumulator
    static uint8_t nmeaLinePos;
    static uint32_t totalBytesProcessed;
    
    static void processSerial();
    static void updateData();
    static void pushNmeaLine();
};
