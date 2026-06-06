// Warhog Mode implementation - Refactored "GPS as Gate" architecture
// 
// Key changes from original:
// - No entries[] vector - data goes directly to disk
// - No "waiting for GPS" state - either GPS or ML-only
// - Simpler memory management - Bloom filter for duplicate detection
// - Per-network file writes instead of batch saves

#include "warhog.h"
#include "oink.h"
#include "../build_info.h"
#include "../core/config.h"
#include "../core/wifi_utils.h"
#include "../core/heap_policy.h"
#include "../core/heap_health.h"
#include "../core/network_recon.h"
#include "../core/wsl_bypasser.h"
#include "../core/sdlog.h"
#include "../core/sd_layout.h"
#include "../core/xp.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
// No M5Cardputer on ESP32-S3 Mini
#include <WiFi.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>

// Bloom filter for seen BSSIDs (fixed memory, no heap churn)
// 4KB = 32,768 bits -> ~1.5% false positives around 5k entries with 3 hashes
static const size_t SEEN_BLOOM_BYTES = 4096;
static const size_t SEEN_BLOOM_BITS = SEEN_BLOOM_BYTES * 8;
static const size_t SEEN_BLOOM_MASK = SEEN_BLOOM_BITS - 1;
static const uint8_t SEEN_BLOOM_HASHES = 3;
static_assert((SEEN_BLOOM_BITS & (SEEN_BLOOM_BITS - 1)) == 0, "SEEN_BLOOM_BITS must be power of two");

// Captured bloom for bounty exclusion (small, fast)
static const size_t CAPTURED_BLOOM_BYTES = 2048;
static const size_t CAPTURED_BLOOM_BITS = CAPTURED_BLOOM_BYTES * 8;
static const size_t CAPTURED_BLOOM_MASK = CAPTURED_BLOOM_BITS - 1;
static const uint8_t CAPTURED_BLOOM_HASHES = 3;
static_assert((CAPTURED_BLOOM_BITS & (CAPTURED_BLOOM_BITS - 1)) == 0, "CAPTURED_BLOOM_BITS must be power of two");

// Bounty pool (reservoir sample of seen networks)
static const size_t BOUNTY_POOL_SIZE = 50;

// Heap threshold for emergency cleanup (bytes) - centralized in HeapPolicy

// Minimum scan interval to avoid tight-loop scanning
static const uint32_t SCAN_INTERVAL_MIN_MS = 1000;

// SD card retry settings (SD can be busy with other operations)
static const int SD_RETRY_COUNT = 3;
static const int SD_RETRY_DELAY_MS = 10;

// WiGLE file size limit for upload compatibility (400KB - leave room for headers)
// Files larger than this will be rotated to a new file
static const size_t WIGLE_FILE_MAX_SIZE = 400000;

// Graceful stop request flag for background scan task
static volatile bool stopRequested = false;
// Set by scan task just before self-deleting, used for safe cleanup in stop()
static volatile bool scanTaskExited = false;

// Helper: Open SD file with retry logic
static File openFileWithRetry(const char* path, const char* mode) {
    File f;
    for (int retry = 0; retry < SD_RETRY_COUNT; retry++) {
        f = SD.open(path, mode);
        if (f) return f;
        delay(SD_RETRY_DELAY_MS);
    }
    return f;  // Returns invalid File if all retries failed
}

// Haversine formula for GPS distance calculation
static double haversineMeters(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;  // Earth radius in meters
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    lat1 = lat1 * M_PI / 180.0;
    lat2 = lat2 * M_PI / 180.0;
    
    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(lat1) * cos(lat2) * sin(dLon / 2) * sin(dLon / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return R * c;
}

// Distance tracking state
static double lastGPSLat = 0;
static double lastGPSLon = 0;
static uint32_t lastDistanceCheck = 0;

// Static members
bool WarhogMode::running = false;
uint32_t WarhogMode::lastScanTime = 0;
uint32_t WarhogMode::scanInterval = 5000;
static uint8_t seenBloom[SEEN_BLOOM_BYTES];
static uint8_t capturedBloom[CAPTURED_BLOOM_BYTES];
static uint64_t bountyPool[BOUNTY_POOL_SIZE];
static uint16_t bountyPoolCount = 0;
static uint32_t bountySeenTotal = 0;
uint32_t WarhogMode::totalNetworks = 0;
uint32_t WarhogMode::openNetworks = 0;
uint32_t WarhogMode::wepNetworks = 0;
uint32_t WarhogMode::wpaNetworks = 0;
uint32_t WarhogMode::savedCount = 0;      // Geotagged networks (CSV)
char WarhogMode::currentFilename[128] = {0};
char WarhogMode::currentWigleFilename[128] = {0};

// Scan state
bool WarhogMode::scanInProgress = false;
uint32_t WarhogMode::scanStartTime = 0;

// Background scan task statics
TaskHandle_t WarhogMode::scanTaskHandle = NULL;
volatile int WarhogMode::scanResult = -2;  // -2 = not started, -1 = running, >=0 = complete

// Scan task check: returns true if should abort
static inline bool shouldAbortScan() {
    return stopRequested || !WarhogMode::isRunning();
}

static uint32_t mix32(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

static bool bloomTest(const uint8_t* bloom, size_t mask, uint8_t hashes, uint64_t key) {
    uint32_t h1 = mix32(key);
    uint32_t h2 = mix32(key ^ 0x9e3779b97f4a7c15ULL) | 1U;
    for (uint8_t i = 0; i < hashes; i++) {
        uint32_t idx = (h1 + (uint32_t)i * h2) & (uint32_t)mask;
        if ((bloom[idx >> 3] & (1 << (idx & 7))) == 0) {
            return false;
        }
    }
    return true;
}

static void bloomAdd(uint8_t* bloom, size_t mask, uint8_t hashes, uint64_t key) {
    uint32_t h1 = mix32(key);
    uint32_t h2 = mix32(key ^ 0x9e3779b97f4a7c15ULL) | 1U;
    for (uint8_t i = 0; i < hashes; i++) {
        uint32_t idx = (h1 + (uint32_t)i * h2) & (uint32_t)mask;
        bloom[idx >> 3] |= (1 << (idx & 7));
    }
}

static void resetSeenTracking() {
    memset(seenBloom, 0, sizeof(seenBloom));
    memset(capturedBloom, 0, sizeof(capturedBloom));
    bountyPoolCount = 0;
    bountySeenTotal = 0;
}

static void seedCapturedFromOink() {
    for (const auto& hs : OinkMode::getHandshakes()) {
        bloomAdd(capturedBloom, CAPTURED_BLOOM_MASK, CAPTURED_BLOOM_HASHES, bssidToKey(hs.bssid));
    }
    for (const auto& p : OinkMode::getPMKIDs()) {
        bloomAdd(capturedBloom, CAPTURED_BLOOM_MASK, CAPTURED_BLOOM_HASHES, bssidToKey(p.bssid));
    }
}

static uint32_t clampScanIntervalMs(uint32_t intervalMs) {
    return (intervalMs < SCAN_INTERVAL_MIN_MS) ? SCAN_INTERVAL_MIN_MS : intervalMs;
}

// Helper to write CSV-escaped SSID field (quoted, doubles internal quotes, strips control chars)
static void writeCSVField(File& f, const char* ssid) {
    f.print("\"");
    for (int i = 0; i < 32 && ssid[i]; i++) {
        if (ssid[i] == '"') {
            f.print("\"\"");
        } else if (ssid[i] >= 32) {  // Skip control characters (newlines, etc)
            f.print(ssid[i]);
        }
    }
    f.print("\"");
}

void WarhogMode::init() {
    totalNetworks = 0;
    openNetworks = 0;
    wepNetworks = 0;
    wpaNetworks = 0;
    savedCount = 0;
    currentFilename[0] = '\0';
    currentWigleFilename[0] = '\0';

    resetSeenTracking();

    scanInterval = clampScanIntervalMs(Config::gps().updateInterval * 1000UL);
}

void WarhogMode::start() {
    if (running) return;

    // Clear previous session data
    totalNetworks = 0;
    openNetworks = 0;
    wepNetworks = 0;
    wpaNetworks = 0;
    savedCount = 0;
    currentFilename[0] = '\0';
    currentWigleFilename[0] = '\0';

    resetSeenTracking();
    seedCapturedFromOink();

    // Reset distance tracking for XP
    lastGPSLat = 0;
    lastGPSLon = 0;
    lastDistanceCheck = 0;
    
    // Reload scan interval from config
    scanInterval = clampScanIntervalMs(Config::gps().updateInterval * 1000UL);
    
    // Reset stop flag for clean start
    stopRequested = false;

    // Stop NetworkRecon before WiFi manipulation (uses promiscuous mode, incompatible with STA scanning)
    NetworkRecon::stop();
    
    // Soft WiFi reset — keep driver alive to avoid esp_wifi_init() RX buffer failures
    WiFi.disconnect(false, true);  // Keep driver, erase AP credentials
    delay(200);             // Let it settle
    WiFi.mode(WIFI_STA);    // Station mode for scanning
    
    // Randomize MAC if enabled (stealth)
    if (Config::wifi().randomizeMAC) {
        WSLBypasser::randomizeMAC();
    }
    
    delay(200);             // Let it initialize
    
    // Reset scan state (critical for proper operation after restart)
    scanInProgress = false;
    scanStartTime = 0;

    // Ensure GPS is in continuous mode regardless of software state
    // FIX: Addresses issue where GPS doesn't show until mode restart
    GPS::ensureContinuousMode();
    
    running = true;
    lastScanTime = 0;  // Trigger immediate scan
    
    // Set grass speed for wardriving - animation controlled by GPS lock in update()
    Avatar::setGrassSpeed(200);  // Slower than OINK (~5 FPS)
    Avatar::setGrassMoving(GPS::hasFix());  // Start based on current GPS status
    
    Display::setWiFiStatus(true);
    Mood::onWarhogUpdate();  // Show WARHOG phrase on start
    Mood::setDialogueLock(true);
}


void WarhogMode::stop() {
    if (!running) return;
    
    // Signal task to stop gracefully
    stopRequested = true;
    scanTaskExited = false;

    // Wait briefly for background scan to notice stopRequested
    if (scanInProgress && scanTaskHandle != NULL) {
        // Give task up to 500ms to exit gracefully
        for (int i = 0; i < 10 && scanTaskHandle != NULL; i++) {
            delay(50);
        }
        // Force cleanup if task didn't exit in time
        if (scanTaskHandle != NULL) {
            Serial.println("[WARHOG] Force-deleting scan task");
            vTaskDelete(scanTaskHandle);
            scanTaskHandle = NULL;
        }
        // Only call scanDelete if task exited cleanly (not mid-scan-processing)
        if (scanTaskExited) {
            WiFi.scanDelete();
        } else {
            // Task was force-killed — WiFi state may be inconsistent.
            // Soft reset keeps driver alive (avoid RX buffer realloc on fragmented heap).
            WiFi.disconnect(false, true);
            delay(50);
        }
    }
    scanInProgress = false;
    scanResult = -2;
    
    // Stop grass animation
    Avatar::setGrassMoving(false);
    
    running = false;
    
    // Put GPS to sleep if power management enabled
    if (Config::gps().powerSave) {
        GPS::sleep();
    }
    
    // Restart NetworkRecon (restores promiscuous mode for OINK/DNH/etc)
    NetworkRecon::start();
    Display::setWiFiStatus(true);  // Recon is active
    
    // Reset stop flag for next run
    stopRequested = false;
    Mood::setDialogueLock(false);
}

// Background task for WiFi scanning - runs sync scan without blocking main loop
void WarhogMode::scanTask(void* pvParameters) {
    // Check for early abort request
    if (shouldAbortScan()) {
        scanResult = -2;
        scanTaskExited = true;
        scanTaskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Soft WiFi reset — keep driver alive to avoid RX buffer realloc failures
    WiFi.scanDelete();
    WiFi.disconnect(false, true);
        vTaskDelay(pdMS_TO_TICKS(100));

    // Check abort between WiFi operations
    if (shouldAbortScan()) {
        scanResult = -2;
        scanTaskExited = true;
        scanTaskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    WiFi.mode(WIFI_STA);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Sync scan - this blocks until complete (which is fine in background task)
    int result = WiFi.scanNetworks(false, true);  // sync, show hidden
    
    // Store result for main loop to pick up
    scanResult = result;

    // Log stack usage for sizing decisions
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    Serial.printf("[WARHOG] Scan task stack HWM: %u bytes unused of 4096\n",
                  (unsigned)(hwm * sizeof(StackType_t)));

    // Signal clean exit, then self-delete
    scanTaskExited = true;
    scanTaskHandle = NULL;
    vTaskDelete(NULL);
}

void WarhogMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    static uint32_t lastPhraseTime = 0;
    static bool lastGPSState = false;
    static uint32_t lastHeapCheck = 0;
    
    // Periodic heap monitoring (every 30 seconds)
    if (now - lastHeapCheck >= 30000) {
        if (HeapHealth::getPressureLevel() >= HeapPressureLevel::Critical) {
            Display::showToast("LOW MEMORY!");
        }
        lastHeapCheck = now;
    }

    // Update grass animation based on GPS fix status
    bool hasGPSFix = GPS::hasFix();
    if (hasGPSFix != lastGPSState) {
        Avatar::setGrassMoving(hasGPSFix);
        lastGPSState = hasGPSFix;
    }
    
    // Distance tracking for XP (every 5 seconds when GPS is available)
    if (hasGPSFix && now - lastDistanceCheck >= 5000) {
        GPSData gps = GPS::getData();
        if (lastGPSLat != 0 && lastGPSLon != 0) {
            double distance = haversineMeters(lastGPSLat, lastGPSLon, gps.latitude, gps.longitude);
            // Filter out GPS jitter (<5m) and teleportation (>1km)
            if (distance > 5.0 && distance < 1000.0) {
                XP::addDistance((uint32_t)distance);
            }
        }
        lastGPSLat = gps.latitude;
        lastGPSLon = gps.longitude;
        lastDistanceCheck = now;
    }
    
    // Rotate phrases every 5 seconds when idle
    if (now - lastPhraseTime >= 5000) {
        Mood::onWarhogUpdate();
        lastPhraseTime = now;
    }
    
    // Check if background scan task is complete
    if (scanInProgress) {
        if (scanResult >= 0) {
            // Scan done
            scanInProgress = false;
            processScanResults();
            scanResult = -2;  // Reset for next scan
        } else if (scanTaskHandle == NULL && scanResult == -2) {
            // Task ended but no result - something went wrong
            scanInProgress = false;
        } else if (now - scanStartTime > 20000) {
            // Timeout after 20 seconds
            if (scanTaskHandle != NULL) {
                vTaskDelete(scanTaskHandle);
                scanTaskHandle = NULL;
            }
            scanInProgress = false;
            scanResult = -2;
            WiFi.scanDelete();
        }
        // Still running - just return (UI stays responsive)
        return;
    }
    
    // Start new scan if interval elapsed and not already scanning
    if (now - lastScanTime >= scanInterval) {
        performScan();
        lastScanTime = now;
    }
}

void WarhogMode::triggerScan() {
    if (!scanInProgress) {
        performScan();
    }
}

bool WarhogMode::isScanComplete() {
    return !scanInProgress && scanResult >= 0;
}

void WarhogMode::performScan() {
    if (scanInProgress) return;
    if (scanTaskHandle != NULL) return;  // Previous task still running

    scanInProgress = true;
    scanStartTime = millis();
    scanResult = -1;  // Running
    scanTaskExited = false;

    // Create background task for sync scan
    xTaskCreatePinnedToCore(
        scanTask,           // Function
        "wifiScan",         // Name
        4096,               // Stack size
        NULL,               // Parameters
        1,                  // Priority (low)
        &scanTaskHandle,    // Task handle
        0                   // Run on core 0 (WiFi core)
    );
    
    if (scanTaskHandle == NULL) {
        // Fallback: run sync scan on main thread if task creation fails
        scanInProgress = false;
        scanResult = WiFi.scanNetworks(false, true);
        if (scanResult >= 0) {
            processScanResults();
        }
        scanResult = -2;
    }
}

// Ensure CSV file exists with header
bool WarhogMode::ensureCSVFileReady() {
    if (currentFilename[0] != '\0') return true;

    // Ensure wardriving directory exists
    const char* wardrivingDir = SDLayout::wardrivingDir();
    if (!SD.exists(wardrivingDir)) {
        if (!SD.mkdir(wardrivingDir)) {
            return false;
        }
    }

    generateFilename(currentFilename, sizeof(currentFilename), "csv");

    File f = openFileWithRetry(currentFilename, FILE_WRITE);
    if (!f) {
        currentFilename[0] = '\0';
        return false;
    }
    
    f.println("BSSID,SSID,RSSI,Channel,AuthMode,Latitude,Longitude,Altitude,Timestamp");
