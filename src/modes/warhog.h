// Warhog Mode - Wardriving with GPS
// Refactored "GPS as Gate" architecture - no entries[] accumulation
#pragma once

#include <Arduino.h>
#include <vector>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../gps/gps.h"

// BSSID key for map lookup (6 bytes as uint64_t)
inline uint64_t bssidToKey(const uint8_t* bssid) {
    return ((uint64_t)bssid[0] << 40) | ((uint64_t)bssid[1] << 32) |
           ((uint64_t)bssid[2] << 24) | ((uint64_t)bssid[3] << 16) |
           ((uint64_t)bssid[4] << 8) | bssid[5];
}

// PSRAM ring buffer entry (compact binary, ~76 bytes)
struct WarhogEntry {
    uint8_t bssid[6];
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t auth;        // wifi_auth_mode_t cast to uint8
    double lat;
    double lon;
    double alt;
    float accuracy;
    uint32_t timestamp;  // millis() at capture
};

static constexpr size_t WARHOG_RING_SIZE = 4096;
static constexpr size_t WARHOG_RING_CAPACITY = WARHOG_RING_SIZE / sizeof(WarhogEntry);

class WarhogMode {
public:
    static constexpr uint8_t MAX_BOUNTIES = 15;  // Max bounty targets to send per payload

    static void init();
    static void start();
    static void stop();
    static void update();
    static bool isRunning() { return running; }
    
    // Scan control
    static void triggerScan();
    static bool isScanComplete();
    
    // Export (data already on disk, these are for format info)
    static bool exportCSV(const char* path);
    
    // GPS
    static bool hasGPSFix();
    static GPSData getGPSData();
    
    // Statistics
    static uint32_t getTotalNetworks() { return totalNetworks; }
    static uint32_t getOpenNetworks() { return openNetworks; }
    static uint32_t getWEPNetworks() { return wepNetworks; }
    static uint32_t getWPANetworks() { return wpaNetworks; }
    static uint32_t getSavedCount() { return savedCount; }  // Geotagged networks (CSV)

    // === BOUNTY SYSTEM (Phase 5) ===
    static void markCaptured(const uint8_t* bssid);                   // Track captures to exclude from bounties
    static void buildBountyList(uint8_t* buffer, uint8_t* count);     // Populate bounty payload buffer (max 15)
    static std::vector<uint64_t> getUnclaimedBSSIDs();                // Random sample of seen, excluding captured

private:
    static bool running;
    static uint32_t lastScanTime;
    static uint32_t scanInterval;
    static bool scanInProgress;
    static uint32_t scanStartTime;
    
    // Statistics
    static uint32_t totalNetworks;   // All unique networks seen
    static uint32_t openNetworks;
    static uint32_t wepNetworks;
    static uint32_t wpaNetworks;
    static uint32_t savedCount;      // Networks saved with GPS to CSV
    static char currentFilename[128];   // Current session CSV file
    static char currentWigleFilename[128]; // Current session WiGLE CSV file

    // Background scan task
    static TaskHandle_t scanTaskHandle;
    static volatile int scanResult;
    
    // PSRAM write buffer
    static WarhogEntry* ringBuf;
    static uint16_t ringHead;
    static uint16_t ringCount;
    static uint32_t lastFlushTime;
    static void flushBuffer();
    static void pushEntry(const WarhogEntry& entry);
    
    static void performScan();
    static void scanTask(void* pvParameters);
    static void processScanResults();
    
    // File helpers - write directly per-network
    static bool ensureCSVFileReady();
    static bool ensureWigleFileReady();
    static void checkWigleFileRotation();
    static void appendCSVEntry(const uint8_t* bssid, const char* ssid,
                               int8_t rssi, uint8_t channel, wifi_auth_mode_t auth,
                               double lat, double lon, double alt);
    static void appendWigleEntry(const uint8_t* bssid, const char* ssid,
                                 int8_t rssi, uint8_t channel, wifi_auth_mode_t auth,
                                 double lat, double lon, double alt, double accuracy);
    static void appendCSVEntryBatch(const WarhogEntry& e);
    static void appendWigleEntryBatch(const WarhogEntry& e);
    
    static const char* authModeToString(wifi_auth_mode_t mode);
    static const char* authModeToWigleString(wifi_auth_mode_t mode);
    static void generateFilename(char* buf, size_t bufSize, const char* ext);
};
