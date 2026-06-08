// Main entry point for ESP32-S3 Mini
// by 0ct0 | ported to ESP32-S3 Mini

#include "hal/hal_input.h"
#include "hal/hal_display.h"
#include "hal/hal_audio.h"
#include "hal/hal_battery.h"
#include "hal/hal_neopixel.h"
#include "hal/hal_pins.h"
#include <SD.h>
#include <WiFi.h>              // <-- PATCH: init WiFi early (before heap fragmentation)
#include <esp_heap_caps.h>     // For heap conditioning
#include <nvs_flash.h>         // For NVS init + recovery at boot
#include <string.h>            // For memset
#include "core/porkchop.h"
#include "core/config.h"
#include "core/xp.h"
#include "core/sdlog.h"
#include "core/wifi_utils.h"
#include "core/heap_policy.h"
#include "core/heap_health.h"
#include "core/network_recon.h"
#include "ui/display.h"
#include "gps/gps.h"
#include "piglet/avatar.h"
#include "piglet/mood.h"
#include "modes/oink.h"
#include "modes/warhog.h"
#include "audio/sfx.h"

Porkchop porkchop;

// --- PATCH: Pre-init WiFi driver early to avoid later esp_wifi_init() failures
// Some reconnect flows (and some Arduino/M5 stacks) end up deinit/reinit WiFi later.
// If heap is fragmented by display sprites / big allocations, esp_wifi_init() may fail with:
//   "Expected to init 4 rx buffer, actual is X" and "wifiLowLevelInit(): esp_wifi_init 257"
static void preInitWiFiDriverEarly() {
    WiFi.persistent(false);

    // Force driver/buffers allocation while heap is still clean/contiguous
    WiFi.mode(WIFI_STA);

    // Stop radio but keep driver initialized (buffers stay allocated).
    // Signature: disconnect(bool wifioff, bool eraseap)
    WiFi.disconnect(true /* wifioff */, false /* eraseap */);

    // No modem sleep to reduce odd timing/latency during TLS + UI load
    WiFi.setSleep(false);

    delay(HeapPolicy::kWiFiModeDelayMs);
}

// Reservation Fence: Force WiFi driver allocations to the TOP of heap,
// leaving a large contiguous region below for application use.
//
// Why this works: TLSF's good-fit strategy allocates from the lowest
// available block. By occupying the bottom 80KB with a fence, the WiFi
// driver's ~35KB of permanent DMA/RX buffers land above the fence.
// When we free the fence, the bottom 80KB is contiguous free space.
//
// This replaces the old 5-phase alloc/free conditioning dance with a
// deterministic, 3-line pattern that's both simpler and more effective.
static void setupHeapLayout() {
    size_t beforeFree = ESP.getFreeHeap();
    size_t beforeLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    Serial.printf("[BOOT] Pre-fence heap: free=%u largest=%u\n",
                  (unsigned)beforeFree, (unsigned)beforeLargest);

    // Allocate fence to push WiFi driver allocations high in the heap
    static constexpr size_t kFenceSize = 80000;
    void* fence = heap_caps_malloc(kFenceSize, MALLOC_CAP_8BIT);
    if (fence) {
        Serial.printf("[BOOT] Fence allocated: %u bytes at %p\n",
                      (unsigned)kFenceSize, fence);
    } else {
        Serial.println("[BOOT] WARNING: Fence allocation failed, falling back to direct init");
    }

    // WiFi driver allocates its permanent DMA/RX buffers ABOVE the fence
    preInitWiFiDriverEarly();

    // Release the fence — leaves large contiguous space below WiFi driver
    if (fence) {
        heap_caps_free(fence);
    }

    size_t afterFree = ESP.getFreeHeap();
    size_t afterLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    Serial.printf("[BOOT] Post-fence heap: free=%u largest=%u\n",
                  (unsigned)afterFree, (unsigned)afterLargest);
}

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n=== PORKCHOP STARTING (ESP32-S3 Mini) ===\n");

    // NVS init — must run before Preferences, Config, XP, or anything using NVS.
    // Recover automatically if the partition has stale data from a previous flash layout.
    {
        esp_err_t nvsErr = nvs_flash_init();
        if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            Serial.printf("[BOOT] NVS corrupt (0x%x), erasing and reinitializing...\n", nvsErr);
            nvs_flash_erase();
            nvsErr = nvs_flash_init();
        }
        Serial.printf("[BOOT] NVS init: %s\n", nvsErr == ESP_OK ? "OK" : esp_err_to_name(nvsErr));
    }

    // Init hal_gpio_setup — configure pins (display, input, audio, etc.)
    hal_gpio_setup();

    // Init display early for boot messages
    hal_display_init();
    hal_display_setBrightness(200);  // Initial brightness using ESP-IDF LEDC API

    // Init NeoPixel
    hal_neopixel_init();

    // Init battery ADC
    hal_battery_init();

    // Init audio early (piezo on GPIO 7)
    hal_audio_init();

    // Reservation fence: push WiFi driver allocations high in heap, then free
    // the fence to leave large contiguous space at the bottom.
    setupHeapLayout();

    // Load configuration from SD
    if (!Config::init()) {
        Serial.println("[MAIN] Config init failed, using defaults");
    }

    // Init SD logging (will be enabled via settings if user wants)
    SDLog::init();

    // Load previous session watermarks before resetting peaks
    HeapHealth::loadPreviousSession();

    // TLS reserve disabled: browser handles TLS, keep heap for UI/file transfer.

    // Init display system
    Display::init();

    // Init audio early so boot sound plays
    SFX::init();

    // Show boot splash (3 screens: OINK OINK, MY NAME IS, PORKCHOP)
    Display::showBootSplash();

    // Apply saved brightness
    hal_display_setBrightness(Config::personality().brightness * 255 / 100);

    // Initialize piglet personality
    Avatar::init();
    Mood::init();

    // Initialize GPS (if enabled)
    if (Config::gps().enabled) {
        // Use fixed pins from board configuration (GPIO TX=1, RX=2)
        GPS::init(PIN_GPS_TX, PIN_GPS_RX, Config::gps().baudRate);
    }

    // Initialize modes
    OinkMode::init();
    WarhogMode::init();
    porkchop.init();

    Serial.println("=== PORKCHOP READY ===");
    Serial.printf("Piglet: %s\n", Config::personality().name);
    Serial.printf("[BOOT] After init: free=%u\n", (unsigned)ESP.getFreeHeap());
    
    // Start background network reconnaissance service
    // This stabilizes heap by running WiFi promiscuous mode early
    // and provides shared network data for OINK/DONOHAM/SPECTRUM modes
    NetworkRecon::start();

    // Reset heap health baseline to post-init state so the health bar
    // starts at the REAL value, not 100%. Without this, the EMA slowly
    // converges from 100% to reality, looking like a steady decline.
    HeapHealth::resetPeaks(true);
}

void loop() {
    hal_input_update();

    // Persist session watermarks to SD (rate-limited to 60s internally)
    HeapHealth::persistWatermarks();

    {
        static bool bakedActive = false;
        static uint32_t bakedStartMs = 0;
        static uint32_t bakedDurationMs = 0;
        static bool bakedTriggered = false;
        static uint32_t lastBakedCheck = 0;

        if (bakedActive) {
            if (millis() - bakedStartMs >= bakedDurationMs) {
                bakedActive = false;
            } else {
                yield();
                return;
            }
        }

        if (!bakedTriggered && XP::hasUnlockable(3) && millis() - lastBakedCheck > 1000) {
            lastBakedCheck = millis();
            time_t now = time(nullptr);
            if (now > 1600000000) {
                int8_t tzOffset = Config::gps().timezoneOffset;
                now += (int32_t)tzOffset * 3600;
                struct tm timeinfo;
                gmtime_r(&now, &timeinfo);
                if ((timeinfo.tm_hour == 4 || timeinfo.tm_hour == 16) && timeinfo.tm_min == 20) {
                    bakedActive = true;
                    bakedStartMs = millis();
                    bakedDurationMs = random(120000, 420001);
                    bakedTriggered = true;
                }
            }
        }
    }

    // Update GPS
    if (Config::gps().enabled) {
        GPS::update();
    }

    // Update mood system
    Mood::update();

    // Update main controller (handles modes, input, state)
    porkchop.update();

    // Update display
    Display::update();

    // Slower update rate for smoother animation
}
