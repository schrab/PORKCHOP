// NetworkRecon - Background WiFi Reconnaissance Service
// Provides shared network scanning for OINK, DONOHAM, and SPECTRUM modes
// Stabilizes heap at boot by running promiscuous mode early
#pragma once

#include <Arduino.h>
#include <esp_wifi.h>
#include <vector>

// Maximum networks to track
#define MAX_RECON_NETWORKS 200

// Channel hop order (2.4GHz - most common first)
#define RECON_CHANNEL_COUNT 13

// Heap stabilization typically happens within this time
#define HEAP_STABILIZE_TIMEOUT_MS 500

// Forward declarations - full structs defined in oink.h for now
// TODO: Move these to a shared types header after full migration
struct DetectedClient;
struct DetectedNetwork;

namespace NetworkRecon {

// ============================================================================
// Lifecycle Management
// ============================================================================

/**
 * @brief Initialize the recon service (call once at boot)
 * Sets up mutexes, reserves vector capacity
 */
void init();

/**
 * @brief Start background WiFi promiscuous scanning
 * Enables WiFi, sets up callback, starts channel hopping
 * Heap stabilizes ~115ms after first packets received
 */
void start();

/**
 * @brief Full stop - disables WiFi promiscuous mode
 * Used when entering BLE modes (PIGGYBLUES) that need WiFi OFF
 */
void stop();

/**
 * @brief Release the networks vector memory entirely
 * Call after stop() when entering modes that don't use recon data (FILE_TRANSFER).
 * start() will re-reserve and rescan on mode exit.
 */
void freeNetworks();

/**
 * @brief Pause promiscuous mode but keep WiFi STA active
 * Used when entering ESP-NOW modes (PIGSYNC) that conflict with promiscuous
 */
void pause();

/**
 * @brief Resume promiscuous mode after pause
 * Restores scanning after ESP-NOW mode exits
 */
void resume();

/**
 * @brief Suspend the promiscuous RX callback without disabling promiscuous mode.
 * Lightweight, non-blocking: just sets esp_wifi_set_promiscuous_rx_cb(nullptr).
 * The WiFi task continues running (TG1 ISR stays alive) but no callbacks fire.
 * Safe from Core 0 contexts where pause() would block the WiFi driver → TG1WDT.
 * @warning Caller MUST call restoreRxCallback() after the critical section.
 */
void suspendRxCallback();

/**
 * @brief Restore the promiscuous RX callback after suspendRxCallback().
 * Re-registers NetworkRecon's promiscuousCallback with the WiFi driver.
 */
void restoreRxCallback();

/**
 * @brief Called every loop iteration
 * Handles channel hopping, stale network cleanup, deferred event processing
 */
void update();

// ============================================================================
// State Queries
// ============================================================================

/**
 * @brief Check if recon is actively scanning
 */
bool isRunning();

/**
 * @brief Check if recon is paused (WiFi on but promiscuous off)
 */
bool isPaused();

/**
 * @brief Check if it's safe to transmit frames right now
 * Returns false if recon is not running, paused, or WiFi driver not ready
 */
bool canSendFramesNow();

/**
 * @brief Check if heap has stabilized after start
 * Returns true once largest free block exceeds threshold
 */
bool isHeapStable();

/**
 * @brief Get current scanning channel
 */
uint8_t getCurrentChannel();
uint32_t getHopIntervalMs();

/**
 * @brief Override channel hop interval (0 = clear override)
 */
void setHopIntervalOverride(uint32_t intervalMs);
void clearHopIntervalOverride();

/**
 * @brief Get packet count since start
 */
uint32_t getPacketCount();

/**
 * @brief Get Core 0 packet count (incremented in promiscuous callback)
 * Used by OINK diag emitter to detect Core 0 stall before TG1WDT.
 */
uint32_t getCore0PacketCount();

// ============================================================================
// Quality + Client Estimates
// ============================================================================

/**
 * @brief Approximate unique client count for a network
 * Uses a small bitset updated from data frames (lower-bound estimate).
 */
uint8_t estimateClientCount(const DetectedNetwork& net);

/**
 * @brief Compute a 0-100 quality score for a network
 * Combines RSSI (smoothed), recency, activity, and beacon stability.
 */
uint8_t getQualityScore(const DetectedNetwork& net);

// ============================================================================
// Shared Network Data Access
// ============================================================================

/**
 * @brief Get reference to shared networks vector
 * Thread-safe access via internal mutex
 * @warning Do not hold reference across yield() calls
 */
std::vector<DetectedNetwork>& getNetworks();

/**
 * @brief Get network count
 */
uint16_t getNetworkCount();

/**
 * @brief Find network by BSSID and copy to output buffer
 * @param bssid The BSSID to search for
 * @param out Output buffer for network copy (can be nullptr to just check existence)
 * @return true if found, false if not found
 * @note Copies data while holding lock - safe for caller to use after return
 */
bool findNetwork(const uint8_t* bssid, DetectedNetwork* out);

/**
 * @brief Find network index by BSSID
 * @return Index or -1 if not found
 */
int findNetworkIndex(const uint8_t* bssid);

// ============================================================================
// Channel Control
// ============================================================================

/**
 * @brief Lock to specific channel (for targeted operations)
 * Disables channel hopping until unlocked
 * @note No-op when manual channel lock is active (see setManualChannelLock)
 */
void lockChannel(uint8_t channel);

/**
 * @brief Unlock channel and resume hopping
 * @note No-op when manual channel lock is active
 */
void unlockChannel();

/**
 * @brief Check if channel is locked (by mode or manual)
 */
bool isChannelLocked();

/**
 * @brief Manually set channel (temporary, hopping will override unless locked)
 */
void setChannel(uint8_t channel);

// ============================================================================
// Manual Channel Lock (user-initiated, overrides mode auto-lock)
// ============================================================================

/**
 * @brief Lock channel from user input. Overrides all mode lockChannel/unlockChannel
 * calls until cleared. Used by OINK/DNH UP/DOWN joystick control.
 * @param channel WiFi channel 1-13
 */
void setManualChannelLock(uint8_t channel);

/**
 * @brief Clear manual lock, restore normal mode-driven channel control
 */
void clearManualChannelLock();

/**
 * @brief Check if manual (user) channel lock is active
 */
bool isManualChannelLocked();

/**
 * @brief Get the manually-locked channel (0 if not manually locked)
 */
uint8_t getManualLockedChannel();

// ============================================================================
// Mode-Specific Callbacks
// ============================================================================

/**
 * @brief Packet callback type for mode-specific processing
 * Called for every received packet after basic network tracking
 * @param pkt The promiscuous packet
 * @param type Packet type (MGMT, CTRL, DATA, MISC)
 */
using PacketCallback = void(*)(const wifi_promiscuous_pkt_t* pkt, wifi_promiscuous_pkt_type_t type);

/**
 * @brief Register callback for mode-specific packet processing
 * Only one callback active at a time (last registration wins)
 * Pass nullptr to clear callback
 */
void setPacketCallback(PacketCallback callback);

/**
 * @brief New network discovery callback type
 * Called from update() when a new network is added to the shared vector
 * Safe to call Mood/XP functions from this callback (runs in main loop context)
 * @param authmode WiFi auth mode (OPEN, WPA2, WPA3, etc.)
 * @param isHidden True if network has hidden/empty SSID
 * @param ssid Network SSID (may be empty for hidden)
 * @param rssi Signal strength
 * @param channel WiFi channel
 */
using NewNetworkCallback = void(*)(wifi_auth_mode_t authmode, bool isHidden, 
                                   const char* ssid, int8_t rssi, uint8_t channel);

/**
 * @brief Register callback for new network discovery notifications
 * Called from main loop context (safe for XP/Mood calls)
 * Only one callback active at a time (last registration wins)
 * Pass nullptr to clear callback
 */
void setNewNetworkCallback(NewNetworkCallback callback);

// ============================================================================
// Thread Safety
// ============================================================================

/**
 * @brief Enter critical section for network vector access
 * @warning Must call exitCritical() after, keep critical sections short
 */
void enterCritical();

/**
 * @brief Exit critical section
 */
void exitCritical();

// ============================================================================
// Lock-hold Profiling (debug only — enabled by build flag)
// ============================================================================
//
// Compile-time toggle. When defined, NetworkRecon::enterCritical/exitCritical
// track per-tick max hold time and total hold time. The OINK diag emitter
// reads and resets these each tick. Add to platformio.ini build_flags to use:
//   -DNETRECON_LOCK_PROFILE=1
//
// Disabled by default — adds a few register reads per enter/exit but no
// allocation, no logging, no extra locks.

#ifdef NETRECON_LOCK_PROFILE
namespace Profile {
    // Reset per-tick accumulators. Call at the top of OinkMode::update().
    void lockProfileReset();

    // Snapshot the current per-tick totals. Does NOT reset.
    //   maxHoldUs: longest single enter/exitCritical span this tick (µs)
    //   totalHoldUs: sum of all enter/exitCritical spans this tick (µs)
    //   callCount: number of enter/exitCritical pairs this tick
    void lockProfileSnapshot(uint32_t& maxHoldUs, uint32_t& totalHoldUs, uint32_t& callCount);
}
#endif

/**
 * @brief SSID Cache — BSSID→SSID mapping that persists after networks[] cleanup.
 * Core 1 only (no spinlock needed). LRU eviction at SSID_CACHE_SIZE entries.
 */
#define SSID_CACHE_SIZE 64

struct SsidCacheEntry {
    uint8_t bssid[6];
    char ssid[33];
    uint32_t lastSeen;
};

void updateSsidCache(const uint8_t* bssid, const char* ssid);
bool lookupSsidCache(const uint8_t* bssid, char* ssidOut, size_t maxLen);

/**
 * @brief RAII wrapper for critical section
 */
class CriticalSection {
public:
    CriticalSection() { enterCritical(); }
    ~CriticalSection() { exitCritical(); }
};

} // namespace NetworkRecon
