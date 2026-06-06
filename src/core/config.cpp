// src/core/config.cpp
// Configuration management implementation

#include "config.h"
#include "sdlog.h"
#include "sd_layout.h"
// No M5Cardputer on ESP32-S3 Mini
#include <SD.h>
#include <SPIFFS.h>
#include <SPI.h>
#include <driver/gpio.h>

// ---- Cardputer microSD wiring (explicit, per Cardputer v1.1 schematic) ----
// ESP32-S3FN8:
//   microSD Socket  CS   MOSI  CLK   MISO
//                  G12  G14   G40   G39
//
// (Your previous patch used ESP32 “classic” pins + CS=4, which breaks SD on Cardputer/StampS3.)
static constexpr int SD_CS_PIN   = 12;  // CS
static constexpr int SD_MOSI_PIN = 14;  // MOSI
static constexpr int SD_MISO_PIN = 39;  // MISO
static constexpr int SD_SCK_PIN  = 40;  // SCK/CLK

// Dedicated SPI bus instance for SD.
// Cardputer microSD pinmap (from M5 docs): CS=12 MOSI=14 CLK=40 MISO=39.
// In practice, Arduino-ESP32/PlatformIO combos vary; using FSPI with explicit
// pins is the most reliable on Cardputer builds.
static SPIClass sdSPI(FSPI);
static bool sdSpiBegun = false;

// Static member initialization
GPSConfig Config::gpsConfig;
MLConfig Config::mlConfig;
WiFiConfig Config::wifiConfig;
BLEConfig Config::bleConfig;
PersonalityConfig Config::personalityConfig;
bool Config::initialized = false;
static bool sdAvailable = false;

// ---- Binary config blob (zero heap allocation) ----
static constexpr uint32_t CONFIG_MAGIC   = 0x504F524B;  // 'PORK'
static constexpr uint16_t CONFIG_VERSION = 1;
#define CONFIG_BIN_FILE "/porkchop.dat"

static const char* configBinPathSD() {
    return SDLayout::usingNewLayout()
        ? "/m5porkchop/config/porkchop.dat"
        : "/porkchop.dat";
}

struct __attribute__((packed)) ConfigBlob {
    uint32_t magic;
    uint16_t version;
    uint16_t blobSize;

    // GPS
    uint8_t  gpsEnabled;
    uint8_t  gpsSource;
    uint8_t  gpsRxPin;
    uint8_t  gpsTxPin;
    uint32_t gpsBaudRate;
    uint16_t gpsUpdateInterval;
    uint16_t gpsSleepTimeMs;
    uint8_t  gpsPowerSave;
    int8_t   gpsTimezoneOffset;

    // WiFi
    uint16_t channelHopInterval;
    uint16_t spectrumHopInterval;
    uint16_t lockTime;
    uint8_t  enableDeauth;
    uint8_t  randomizeMAC;
    int8_t   spectrumMinRssi;
    int8_t   attackMinRssi;
    uint8_t  spectrumTopN;
    uint16_t spectrumStaleMs;
    uint8_t  spectrumCollapseSsid;
    uint8_t  spectrumTiltEnabled;
    char     otaSSID[33];
    char     otaPassword[65];
    uint8_t  autoConnect;
    char     wpaSecKey[33];
    char     wigleApiName[65];
    char     wigleApiToken[65];

    // BLE
    uint16_t burstInterval;
    uint16_t advDuration;

    // ML (disabled but preserved for future)
    uint8_t  mlEnabled;
    uint8_t  mlCollectionMode;
    char     mlModelPath[64];
    float    mlConfidenceThreshold;
    float    mlRogueApThreshold;
    float    mlVulnScorerThreshold;
    uint8_t  mlAutoUpdate;
    char     mlUpdateUrl[128];
};

static void populateBlob(ConfigBlob& b, const GPSConfig& gps, const WiFiConfig& wifi,
                          const BLEConfig& ble, const MLConfig& ml) {
    memset(&b, 0, sizeof(b));
    b.magic    = CONFIG_MAGIC;
    b.version  = CONFIG_VERSION;
    b.blobSize = sizeof(ConfigBlob);

    b.gpsEnabled        = gps.enabled ? 1 : 0;
    b.gpsSource         = static_cast<uint8_t>(gps.source);
    b.gpsRxPin          = gps.rxPin;
    b.gpsTxPin          = gps.txPin;
    b.gpsBaudRate       = gps.baudRate;
    b.gpsUpdateInterval = gps.updateInterval;
    b.gpsSleepTimeMs    = gps.sleepTimeMs;
    b.gpsPowerSave      = gps.powerSave ? 1 : 0;
    b.gpsTimezoneOffset = gps.timezoneOffset;

    b.channelHopInterval   = wifi.channelHopInterval;
    b.spectrumHopInterval  = wifi.spectrumHopInterval;
    b.lockTime             = wifi.lockTime;
    b.enableDeauth         = wifi.enableDeauth ? 1 : 0;
    b.randomizeMAC         = wifi.randomizeMAC ? 1 : 0;
    b.spectrumMinRssi      = wifi.spectrumMinRssi;
    b.attackMinRssi        = wifi.attackMinRssi;
    b.spectrumTopN         = wifi.spectrumTopN;
    b.spectrumStaleMs      = wifi.spectrumStaleMs;
    b.spectrumCollapseSsid = wifi.spectrumCollapseSsid ? 1 : 0;
    b.spectrumTiltEnabled  = wifi.spectrumTiltEnabled ? 1 : 0;
    strncpy(b.otaSSID,       wifi.otaSSID,       sizeof(b.otaSSID) - 1);
    strncpy(b.otaPassword,   wifi.otaPassword,   sizeof(b.otaPassword) - 1);
    b.autoConnect = wifi.autoConnect ? 1 : 0;
    strncpy(b.wpaSecKey,     wifi.wpaSecKey,     sizeof(b.wpaSecKey) - 1);
    strncpy(b.wigleApiName,  wifi.wigleApiName,  sizeof(b.wigleApiName) - 1);
    strncpy(b.wigleApiToken, wifi.wigleApiToken, sizeof(b.wigleApiToken) - 1);

    b.burstInterval = ble.burstInterval;
    b.advDuration   = ble.advDuration;

    b.mlEnabled              = ml.enabled ? 1 : 0;
    b.mlCollectionMode       = static_cast<uint8_t>(ml.collectionMode);
    strncpy(b.mlModelPath, ml.modelPath, sizeof(b.mlModelPath) - 1);
    b.mlConfidenceThreshold  = ml.confidenceThreshold;
    b.mlRogueApThreshold     = ml.rogueApThreshold;
    b.mlVulnScorerThreshold  = ml.vulnScorerThreshold;
    b.mlAutoUpdate           = ml.autoUpdate ? 1 : 0;
    strncpy(b.mlUpdateUrl, ml.updateUrl, sizeof(b.mlUpdateUrl) - 1);
}

static bool writeBlobTo(fs::FS& fs, const char* path, const ConfigBlob& b) {
    File file = fs.open(path, FILE_WRITE);
    if (!file) {
        Serial.printf("[CONFIG] writeBlobTo: failed to open '%s'\n", path);
        return false;
    }
    size_t written = file.write((const uint8_t*)&b, sizeof(b));
    file.close();
    Serial.printf("[CONFIG] writeBlobTo: %u/%u bytes -> '%s'\n",
                  written, sizeof(b), path);
    return written == sizeof(b);
}

static bool readBlobFrom(fs::FS& fs, const char* path, ConfigBlob& b) {
    File file = fs.open(path, FILE_READ);
    if (!file) return false;

    size_t fileSize = file.size();
    if (fileSize < 8) { file.close(); return false; }  // too small for header

    size_t readSize = (fileSize < sizeof(b)) ? fileSize : sizeof(b);
    memset(&b, 0, sizeof(b));
    size_t got = file.read((uint8_t*)&b, readSize);
    file.close();

    if (got < 8 || b.magic != CONFIG_MAGIC) return false;
    Serial.printf("[CONFIG] readBlobFrom: '%s' v%u, %u bytes\n", path, b.version, got);
    return true;
}

static void extractBlob(const ConfigBlob& b, GPSConfig& gps, WiFiConfig& wifi,
                         BLEConfig& ble, MLConfig& ml) {
    gps.enabled        = b.gpsEnabled != 0;
    gps.source         = static_cast<GPSSource>(b.gpsSource);
    gps.rxPin          = b.gpsRxPin;
    gps.txPin          = b.gpsTxPin;
    gps.baudRate       = b.gpsBaudRate;
    gps.updateInterval = b.gpsUpdateInterval;
    gps.sleepTimeMs    = b.gpsSleepTimeMs;
    gps.powerSave      = b.gpsPowerSave != 0;
    gps.timezoneOffset = b.gpsTimezoneOffset;

    // Auto-set pins based on source (same as JSON loader)
    if (gps.source == GPSSource::CAP_LORA) {
        gps.rxPin = 15; gps.txPin = 13;
    } else if (gps.source == GPSSource::GROVE) {
        gps.rxPin = 1;  gps.txPin = 2;
    }

    wifi.channelHopInterval   = b.channelHopInterval;
    wifi.spectrumHopInterval  = b.spectrumHopInterval;
    wifi.lockTime             = b.lockTime;
    wifi.enableDeauth         = b.enableDeauth != 0;
    wifi.randomizeMAC         = b.randomizeMAC != 0;
    wifi.spectrumMinRssi      = b.spectrumMinRssi;
    wifi.attackMinRssi        = b.attackMinRssi;
    wifi.spectrumTopN         = b.spectrumTopN;
    wifi.spectrumStaleMs      = b.spectrumStaleMs;
    wifi.spectrumCollapseSsid = b.spectrumCollapseSsid != 0;
    wifi.spectrumTiltEnabled  = b.spectrumTiltEnabled != 0;
    strncpy(wifi.otaSSID,       b.otaSSID,       sizeof(wifi.otaSSID) - 1);
    wifi.otaSSID[sizeof(wifi.otaSSID) - 1] = '\0';
    strncpy(wifi.otaPassword,   b.otaPassword,   sizeof(wifi.otaPassword) - 1);
    wifi.otaPassword[sizeof(wifi.otaPassword) - 1] = '\0';
    wifi.autoConnect = b.autoConnect != 0;
    strncpy(wifi.wpaSecKey,     b.wpaSecKey,     sizeof(wifi.wpaSecKey) - 1);
    wifi.wpaSecKey[sizeof(wifi.wpaSecKey) - 1] = '\0';
    strncpy(wifi.wigleApiName,  b.wigleApiName,  sizeof(wifi.wigleApiName) - 1);
    wifi.wigleApiName[sizeof(wifi.wigleApiName) - 1] = '\0';
    strncpy(wifi.wigleApiToken, b.wigleApiToken, sizeof(wifi.wigleApiToken) - 1);
    wifi.wigleApiToken[sizeof(wifi.wigleApiToken) - 1] = '\0';

    ble.burstInterval = b.burstInterval;
    ble.advDuration   = b.advDuration;

    ml.enabled              = b.mlEnabled != 0;
    ml.collectionMode       = static_cast<MLCollectionMode>(b.mlCollectionMode);
    strncpy(ml.modelPath, b.mlModelPath, sizeof(ml.modelPath) - 1);
    ml.modelPath[sizeof(ml.modelPath) - 1] = '\0';
    ml.confidenceThreshold  = b.mlConfidenceThreshold;
    ml.rogueApThreshold     = b.mlRogueApThreshold;
    ml.vulnScorerThreshold  = b.mlVulnScorerThreshold;
    ml.autoUpdate           = b.mlAutoUpdate != 0;
    strncpy(ml.updateUrl, b.mlUpdateUrl, sizeof(ml.updateUrl) - 1);
    ml.updateUrl[sizeof(ml.updateUrl) - 1] = '\0';
}

static uint16_t clampU16(uint32_t value, uint16_t minVal, uint16_t maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return static_cast<uint16_t>(value);
}

static int8_t clampI8(int value, int8_t minVal, int8_t maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return static_cast<int8_t>(value);
}

static void sanitizeWiFiConfig(WiFiConfig& cfg) {
    cfg.channelHopInterval = clampU16(cfg.channelHopInterval, 50, 2000);
    cfg.spectrumHopInterval = clampU16(cfg.spectrumHopInterval, 50, 2000);
    cfg.spectrumMinRssi = clampI8(cfg.spectrumMinRssi, -95, -30);
    cfg.attackMinRssi = clampI8(cfg.attackMinRssi, -90, -50);
    if (cfg.spectrumTopN > 100) cfg.spectrumTopN = 100;
    cfg.spectrumStaleMs = clampU16(cfg.spectrumStaleMs, 1000, 20000);
}

static void ensureSdSpiReady() {
    // Re-init SD SPI bus cleanly
    if (sdSpiBegun) {
        sdSPI.end();
        sdSpiBegun = false;
        delay(20);
    }

    // Make sure CS is a sane GPIO output and deasserted before touching the bus.
    // This prevents random Select Failed errors on some cards.
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);

    // SCK, MISO, MOSI, SS/CS
    sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    sdSpiBegun = true;
    delay(20);
}

bool Config::init() {
    // Initialize SPIFFS first (always available)
    if (!SPIFFS.begin(false)) {
        Serial.println("[CONFIG] SPIFFS mount failed, attempting format...");
        if (!SPIFFS.begin(true)) {
            Serial.println("[CONFIG] SPIFFS format failed! Personality settings will not persist.");
        } else {
            Serial.println("[CONFIG] SPIFFS formatted and mounted OK");
        }
    }

    // Allow buses to stabilize after HAL init
    delay(50);

    // Ensure SD has a proper SPI bus configured
    ensureSdSpiReady();

    // Retry with progressive SPI speeds for reliability
    sdAvailable = false;
    const int maxRetries = 6;
    const uint32_t speeds[] = {
        25000000, // 25 MHz
        20000000, // 20 MHz
        10000000, // 10 MHz
        8000000,  // 8 MHz
        4000000,  // 4 MHz
        1000000   // 1 MHz
    };

    for (int attempt = 0; attempt < maxRetries && !sdAvailable; attempt++) {
        uint32_t speed = speeds[attempt];
        Serial.printf("[CONFIG] SD init attempt %d/%d at %luMHz\n",
                      attempt + 1, maxRetries, speed / 1000000);

        if (attempt > 0) {
            SD.end();     // Clean up previous failed attempt
            delay(80);    // Allow bus to settle
            ensureSdSpiReady();
        }

        // Use explicit CS + dedicated SPI + explicit speed
        if (SD.begin(SD_CS_PIN, sdSPI, speed)) {
            Serial.printf("[CONFIG] SD card mounted at %luMHz\n", speed / 1000000);
            sdAvailable = true;
        }
    }

    if (!sdAvailable) {
        SDLayout::setUseNewLayout(false);
        Serial.println("[CONFIG] SD card init failed after retries, using SPIFFS");
    } else {
        SDLayout::migrateIfNeeded();
        SDLayout::ensureDirs();
        SDLog::log("CFG", "SD card mounted OK");
    }

    // Load personality from SPIFFS (always available)
    if (!loadPersonality()) {
        Serial.println("[CONFIG] Creating default personality");
        createDefaultPersonality();
        savePersonalityToSPIFFS();
    }

    // Load main config: SD primary, SPIFFS fallback
    Serial.printf("[CONFIG] Pre-load state: sdAvailable=%d, newLayout=%d\n",
                  sdAvailable, SDLayout::usingNewLayout());
    if (!load()) {
        Serial.println("[CONFIG] Creating default config");
        createDefaultConfig();
        save();
    }

    // Try to load keys from files (auto-deletes after import)
    if (loadWpaSecKeyFromFile()) {
        Serial.println("[CONFIG] WPA-SEC key loaded from file");
    }
    if (loadWigleKeyFromFile()) {
        Serial.println("[CONFIG] WiGLE API keys loaded from file");
    }

    // Merge creds from JSON porkchop.conf if present (handles the case where
    // binary config already exists but user dropped a new .conf with creds)
    if (importCredsFromJsonConf()) {
        Serial.println("[CONFIG] Credentials imported from porkchop.conf");
    }

    initialized = true;
    return true;
}

bool Config::isSDAvailable() {
    return sdAvailable;
}

void Config::prepareSDBus() {
    ensureSdSpiReady();
}

SPIClass& Config::sdSpi() {
    return sdSPI;
}

int Config::sdCsPin() {
    return SD_CS_PIN;
}

void Config::prepareCapLoraGpio() {
    // GPIO 13 is ESP32-S3 default FSPIQ (MISO) via IOMUX. Even though SD remaps
    // FSPI MISO to G39, the default IOMUX linkage on G13 can disrupt the FSPI
    // peripheral when Serial2 reconfigures G13 as UART TX output.
    // gpio_reset_pin() clears IOMUX function, disconnects peripheral signals,
    // and returns the pin to plain GPIO mode.
    gpio_reset_pin(static_cast<gpio_num_t>(CapLoraPins::GPS_TX));   // G13

    // Reset SX1262 LoRa chip to known state. The CapLoRa868 LoRa SPI shares
    // MOSI(G14)/MISO(G39)/SCK(G40) with SD card. After reset the SX1262
    // enters STANDBY_RC with all IOs high-impedance, preventing bus contention.
    pinMode(CapLoraPins::LORA_RESET, OUTPUT);
    digitalWrite(CapLoraPins::LORA_RESET, LOW);   // Assert NRESET (active low)
    delay(10);                                      // SX1262 datasheet: >100us
    digitalWrite(CapLoraPins::LORA_RESET, HIGH);   // Release reset
    delay(10);                                      // Wait for standby entry

    // Deassert LoRa chip select (HIGH = not selected, MISO tri-stated)
    pinMode(CapLoraPins::LORA_CS, OUTPUT);
    digitalWrite(CapLoraPins::LORA_CS, HIGH);

    // Configure control pins as inputs (don't drive)
    pinMode(CapLoraPins::LORA_BUSY, INPUT);
    pinMode(CapLoraPins::LORA_DIO1, INPUT);

    Serial.println("[CONFIG] CapLoRa868: SX1262 reset, CS deasserted, G13 IOMUX cleared");
}

bool Config::reinitSD() {
    // Quick check: SD still accessible? Skip destructive reinit if so.
    if (sdAvailable && SD.exists("/")) {
        Serial.println("[CONFIG] SD still accessible after GPS init, skipping reinit");
        return true;
    }

    Serial.println("[CONFIG] SD access lost, attempting re-initialization...");

    // Save current state to restore on failure
    bool wasSdAvailable = sdAvailable;
    bool wasNewLayout = SDLayout::usingNewLayout();

    // Clean up any existing SD state
    SD.end();
    delay(80);

    // Re-init SD SPI bus explicitly
    ensureSdSpiReady();

    // Retry with progressive SPI speeds
    sdAvailable = false;
    const int maxRetries = 6;
    const uint32_t speeds[] = {
        25000000,
        20000000,
        10000000,
        8000000,
        4000000,
        1000000
    };

    for (int attempt = 0; attempt < maxRetries && !sdAvailable; attempt++) {
        uint32_t speed = speeds[attempt];
        Serial.printf("[CONFIG] SD reinit attempt %d/%d at %luMHz\n",
                      attempt + 1, maxRetries, speed / 1000000);

        if (attempt > 0) {
            SD.end();
            delay(80);
            ensureSdSpiReady();
        }

        if (SD.begin(SD_CS_PIN, sdSPI, speed)) {
            Serial.printf("[CONFIG] SD card mounted at %luMHz\n", speed / 1000000);
            sdAvailable = true;
        }
    }

    if (sdAvailable) {
        // Success: verify layout by checking marker directly (no full migration)
        if (SD.exists(SDLayout::migrationMarkerPath())) {
            SDLayout::setUseNewLayout(true);
        } else if (wasNewLayout) {
            // Marker unreadable but we were using new layout — keep it
            SDLayout::setUseNewLayout(true);
        }
        SDLayout::ensureDirs();
        SDLog::log("CFG", "SD card re-initialized OK");
    } else {
        // FAIL: Restore previous state — don't corrupt flags
        sdAvailable = wasSdAvailable;
        SDLayout::setUseNewLayout(wasNewLayout);
        Serial.println("[CONFIG] SD reinit failed, keeping previous SD state");
    }

    return sdAvailable;
}

bool Config::loadFrom(fs::FS& fs, const char* path) {
    File file = fs.open(path, FILE_READ);
    if (!file) return false;

    size_t fileSize = file.size();
    Serial.printf("[CONFIG] loadFrom(): '%s' size=%u bytes\n", path, fileSize);
    if (fileSize == 0) { file.close(); return false; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        Serial.printf("[CONFIG] loadFrom(): JSON error: %s ('%s')\n", err.c_str(), path);
        return false;
    }

    // Populate config from parsed JSON — shared with load()
    return applyJson(doc);
}

bool Config::applyJson(const JsonDocument& doc) {
