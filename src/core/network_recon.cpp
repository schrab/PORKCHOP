// NetworkRecon - Background WiFi Reconnaissance Service Implementation
// Provides shared network scanning for OINK, DONOHAM, and SPECTRUM modes

#include "network_recon.h"
#include "../modes/oink.h"  // For DetectedNetwork, DetectedClient types
#include "config.h"
#include "wsl_bypasser.h"
#include "wifi_utils.h"
#include "heap_gates.h"
#include "heap_policy.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <NimBLEDevice.h>
#include <atomic>

namespace NetworkRecon {

// ============================================================================
// State Variables
// ============================================================================

static bool initialized = false;
static bool running = false;
static bool paused = false;
static std::atomic<bool> channelLocked{false};
static bool channelLockedBeforePause = false;  // [BUG4 FIX] Save state for pause/resume
static uint8_t lockedChannel = 0;
static uint8_t currentChannel = 1;
static uint8_t currentChannelIndex = 0;
static bool manualChannelLock = false;
static uint8_t manualLockedChannel = 0;
static uint32_t lastHopTime = 0;
static uint32_t lastCleanupTime = 0;
static uint32_t startTime = 0;
static std::atomic<uint32_t> packetCount{0};
static std::atomic<bool> busy{false};  // [BUG3 FIX] Atomic for cross-core visibility
static std::atomic<uint32_t> hopIntervalOverrideMs{0};
static bool heapStabilized = false;
// Core 0 heartbeat: incremented in promiscuousCallback (runs on Core 0).
// Read by OINK diag emitter on Core 1 to detect Core 0 stall before TG1WDT.
static std::atomic<uint32_t> s_core0PktCount{0};
// shrinkDeferCount removed — shrink_to_fit no longer runs during operation

// SSID cache: dynamically allocated from internal RAM in start(), freed in stop().
// Must be in internal RAM (not PSRAM) to avoid TG1WDT from dual-core bus contention.
// Declared here (before start()/stop() use it).
static SsidCacheEntry* s_ssidCache = nullptr;

// Channel hop order (most common channels first for faster discovery)
static const uint8_t CHANNEL_HOP_ORDER[RECON_CHANNEL_COUNT] = {
    1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13
};

// Stale network timeout (remove if not seen for this long)
static const uint32_t STALE_TIMEOUT_MS = 60000;

// Cleanup interval
static const uint32_t CLEANUP_INTERVAL_MS = 5000;

// Client activity decay (clear bitset after inactivity)
static const uint32_t CLIENT_BITMAP_RESET_MS = 30000;

static uint32_t getHopIntervalMsInternal() {
    uint32_t overrideMs = hopIntervalOverrideMs.load();
    uint32_t interval = overrideMs > 0 ? overrideMs : Config::wifi().channelHopInterval;
    if (interval < 50) interval = 50;
    if (interval > 2000) interval = 2000;
    return interval;
}

// Beacon interval sanity cap (ignore huge gaps for EMA)
static const uint32_t BEACON_INTERVAL_MAX_MS = 5000;

// ============================================================================
// Thread Safety
// ============================================================================

static portMUX_TYPE vectorMux = portMUX_INITIALIZER_UNLOCKED;

#ifdef NETRECON_LOCK_PROFILE
// Per-tick lock-hold accumulators. Core 1 writes, Core 1 reads — no lock
// needed for the counters themselves, but reads on the diag emitter must
// use the relaxed atomic ordering to avoid seeing torn values.
static std::atomic<uint32_t> s_lockProfileEnterUs{0};
static std::atomic<uint32_t> s_lockProfileMaxUs{0};
static std::atomic<uint32_t> s_lockProfileTotalUs{0};
static std::atomic<uint32_t> s_lockProfileCallCount{0};
static std::atomic<uint8_t>  s_lockProfileInLock{0};

namespace Profile {
    void lockProfileReset() {
        s_lockProfileMaxUs.store(0, std::memory_order_relaxed);
        s_lockProfileTotalUs.store(0, std::memory_order_relaxed);
        s_lockProfileCallCount.store(0, std::memory_order_relaxed);
    }
    void lockProfileSnapshot(uint32_t& maxHoldUs, uint32_t& totalHoldUs, uint32_t& callCount) {
        maxHoldUs   = s_lockProfileMaxUs.load(std::memory_order_relaxed);
        totalHoldUs = s_lockProfileTotalUs.load(std::memory_order_relaxed);
        callCount   = s_lockProfileCallCount.load(std::memory_order_relaxed);
    }
}
#endif // NETRECON_LOCK_PROFILE

// ============================================================================
// Shared Data
// ============================================================================

static std::vector<DetectedNetwork> networks;

// ============================================================================
// Deferred Event Processing (avoid allocations in callback)
// ============================================================================

static const uint8_t PENDING_NET_SLOTS = 16;
static DetectedNetwork pendingNetworks[PENDING_NET_SLOTS];
static std::atomic<uint8_t> pendingNetWrite{0};
static std::atomic<uint8_t> pendingNetRead{0};

// Pending SSID reveal cache (best-effort for hidden networks)
static const uint8_t PENDING_SSID_SLOTS = 4;
struct PendingSSID {
    uint8_t bssid[6];
    char ssid[33];
    std::atomic<bool> ready{false};
};
static PendingSSID pendingSsids[PENDING_SSID_SLOTS];
static std::atomic<uint8_t> pendingSsidWrite{0};

// ============================================================================
// Mode-Specific Callbacks
// ============================================================================

static std::atomic<PacketCallback> modeCallback{nullptr};
static NewNetworkCallback newNetworkCallback = nullptr;

// ============================================================================
// Internal Functions
// ============================================================================

static void hopChannel() {
    if (channelLocked.load(std::memory_order_acquire)) return;
    
    currentChannelIndex = (currentChannelIndex + 1) % RECON_CHANNEL_COUNT;
    currentChannel = CHANNEL_HOP_ORDER[currentChannelIndex];
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
}

static int findNetworkInternal(const uint8_t* bssid) {
    for (size_t i = 0; i < networks.size(); i++) {
        if (memcmp(networks[i].bssid, bssid, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static inline int8_t updateRssiAvg(int8_t prev, int8_t sample) {
    if (prev == 0) return sample;
    int16_t blended = (int16_t)prev * 3 + (int16_t)sample;
    return (int8_t)(blended / 4);
}

static bool enqueuePendingNetwork(const DetectedNetwork& net) {
    uint8_t write = pendingNetWrite.load(std::memory_order_relaxed);
    uint8_t next = (uint8_t)((write + 1) % PENDING_NET_SLOTS);
    uint8_t read = pendingNetRead.load(std::memory_order_acquire);
    if (next == read) {
        return false;  // Queue full, drop
    }
    pendingNetworks[write] = net;
    pendingNetWrite.store(next, std::memory_order_release);
    return true;
}

static bool dequeuePendingNetwork(DetectedNetwork& out) {
    uint8_t read = pendingNetRead.load(std::memory_order_relaxed);
    uint8_t write = pendingNetWrite.load(std::memory_order_acquire);
    if (read == write) {
        return false;
    }
    out = pendingNetworks[read];
    pendingNetRead.store((uint8_t)((read + 1) % PENDING_NET_SLOTS), std::memory_order_release);
    return true;
}

static void storePendingSsid(const uint8_t* bssid, const char* ssid) {
    if (!bssid || !ssid || ssid[0] == 0) return;
    uint8_t slot = pendingSsidWrite.fetch_add(1, std::memory_order_relaxed) % PENDING_SSID_SLOTS;
    pendingSsids[slot].ready.store(false, std::memory_order_relaxed);
    memcpy(pendingSsids[slot].bssid, bssid, 6);
    strncpy(pendingSsids[slot].ssid, ssid, 32);
    pendingSsids[slot].ssid[32] = 0;
    pendingSsids[slot].ready.store(true, std::memory_order_release);
}

static bool applyPendingSsid(DetectedNetwork& net) {
    for (uint8_t i = 0; i < PENDING_SSID_SLOTS; i++) {
        if (!pendingSsids[i].ready.load(std::memory_order_acquire)) continue;
        if (memcmp(pendingSsids[i].bssid, net.bssid, 6) != 0) continue;
        strncpy(net.ssid, pendingSsids[i].ssid, 32);
        net.ssid[32] = 0;
        net.isHidden = false;
        pendingSsids[i].ready.store(false, std::memory_order_release);
        return true;
    }
    return false;
}

static void revealSsidIfKnown(const uint8_t* bssid, const char* ssid) {
    if (!bssid || !ssid || ssid[0] == 0) return;

    // Try to apply to existing network first
    enterCritical();
    int idx = findNetworkInternal(bssid);
    if (idx >= 0 && idx < (int)networks.size()) {
        if (networks[idx].ssid[0] == 0 || networks[idx].isHidden) {
            strncpy(networks[idx].ssid, ssid, 32);
            networks[idx].ssid[32] = 0;
            networks[idx].isHidden = false;
            networks[idx].lastSeen = millis();
        }
        exitCritical();
        return;
    }
    exitCritical();

    // Otherwise, store for when the network is added
    storePendingSsid(bssid, ssid);
}

static inline uint8_t clientHashIndex(const uint8_t* mac) {
    uint32_t h = 2166136261u;  // FNV-1a
    for (int i = 0; i < 6; i++) {
        h ^= mac[i];
        h *= 16777619u;
    }
    return (uint8_t)(h & 0x7F);
}

static int computeRetentionScore(const DetectedNetwork& net, uint32_t now) {
    int8_t rssi = (net.rssiAvg != 0) ? net.rssiAvg : net.rssi;
    int score = 0;

    if (rssi <= -95) score += 0;
    else if (rssi >= -30) score += 60;
    else score += ((int)rssi + 95) * 60 / 65;

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

    if (net.hasHandshake) score -= 20;
    if (net.hasPMF) score -= 15;
    if (net.authmode == WIFI_AUTH_OPEN) score -= 10;
    if (net.ssid[0] == 0 || net.isHidden) score -= 10;
    if (net.cooldownUntil > now) score -= 10;

    return score;
}

// PMF detection result - we need both bits to distinguish WPA3 from WPA2/WPA3 mixed
// jader242 i know you reading dis. pig knows.
struct PmfResult {
    bool capable;   // MFPC (bit 6) - PMF capable (WPA2/WPA3 transitional sets this)
    bool required;  // MFPR (bit 7) - PMF required (pure WPA3 sets both)
};

static PmfResult detectPMF(const uint8_t* payload, uint16_t len) {
    PmfResult result = {false, false};
    uint16_t offset = 36;
    while (offset + 2 < len) {
        uint8_t id = payload[offset];
        uint8_t ieLen = payload[offset + 1];

        if (offset + 2 + ieLen > len) break;

        if (id == 0x30 && ieLen >= 8) {  // RSN IE
            uint16_t rsnOffset = offset + 2;
            uint16_t rsnEnd = rsnOffset + ieLen;
            uint16_t version = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            if (version != 1) {
                offset += 2 + ieLen;
                continue;
            }

            // Walk the RSN IE properly - no shortcuts, no assumptions
            rsnOffset += 6;  // Skip version(2) + group cipher(4)
            if (rsnOffset + 2 > rsnEnd) break;

            uint16_t pairwiseCount = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            rsnOffset += 2 + (pairwiseCount * 4);
            if (rsnOffset + 2 > rsnEnd) break;

            uint16_t akmCount = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            rsnOffset += 2 + (akmCount * 4);
            if (rsnOffset + 2 > rsnEnd) break;

            // RSN Capabilities - IEEE 802.11-2016 Table 9-133
            // Bit 6: MFPC (Management Frame Protection Capable)
            // Bit 7: MFPR (Management Frame Protection Required)
            uint16_t caps = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            result.capable  = (caps >> 6) & 0x01;  // MFPC
            result.required = (caps >> 7) & 0x01;  // MFPR
            break;
        }

        offset += 2 + ieLen;
    }
    return result;
}

static void processBeacon(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    PmfResult pmf = detectPMF(payload, len);
    uint32_t now = millis();
    
    // [BUG1 FIX] Lookup under spinlock - vector can be modified by cleanupStaleNetworks()
    enterCritical();
    int idx = findNetworkInternal(bssid);
    exitCritical();
    
    if (idx < 0) {
        // New network - queue for deferred add
        DetectedNetwork net = {0};
        memcpy(net.bssid, bssid, 6);
        net.rssi = rssi;
        net.rssiAvg = rssi;
        net.channel = currentChannel;
        net.authmode = WIFI_AUTH_OPEN;
        net.firstSeen = now;
        net.lastSeen = now;
        net.lastBeaconSeen = now;
        net.beaconCount = 1;
        net.beaconIntervalEmaMs = 0;
        net.isTarget = false;
        net.hasPMF = pmf.required;
        net.hasHandshake = false;
        net.attackAttempts = 0;
        net.isHidden = false;
        net.lastDataSeen = 0;
        net.cooldownUntil = 0;
        net.clientBitset = 0;
        net.clientBitsetHigh = 0;

        // Parse SSID from IE
        uint16_t offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (offset + 2 + ieLen > len) break;
            
            if (id == 0) {
                if (ieLen > 0 && ieLen <= 32) {
                    memcpy(net.ssid, payload + offset + 2, ieLen);
                    net.ssid[ieLen] = 0;
                    
                    // Check for all-null SSID (hidden)
                    bool allNull = true;
                    for (uint8_t i = 0; i < ieLen; i++) {
                        if (net.ssid[i] != 0) { allNull = false; break; }
                    }
                    if (allNull) {
                        net.isHidden = true;
                    }
                } else if (ieLen == 0) {
                    net.isHidden = true;
                }
                break;
            }
            
            offset += 2 + ieLen;
        }
        
        // Get channel from DS Parameter Set IE
        offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (id == 3 && ieLen == 1) {
                net.channel = payload[offset + 2];
                break;
            }
            
            offset += 2 + ieLen;
        }
        
        // Parse auth mode from RSN (0x30) and WPA vendor (0xDD) IEs
        // RSN + MFPR        = pure WPA3 (PMF required, no fallback)
        // RSN + MFPC only   = WPA2/WPA3 transitional (PMF capable, clients choose)
        // RSN alone          = WPA2
        // WPA vendor alone   = WPA1
        // RSN + WPA vendor   = WPA1/WPA2 mixed
        bool hasRSN = false;
        offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];

            if (offset + 2 + ieLen > len) break;

            if (id == 0x30 && ieLen >= 2) {  // RSN IE
                hasRSN = true;
                net.authmode = WIFI_AUTH_WPA2_PSK;
            } else if (id == 0xDD && ieLen >= 8) {  // Vendor specific
                if (payload[offset + 2] == 0x00 && payload[offset + 3] == 0x50 &&
                    payload[offset + 4] == 0xF2 && payload[offset + 5] == 0x01) {
                    if (!hasRSN) {
                        net.authmode = WIFI_AUTH_WPA_PSK;
                    } else {
                        net.authmode = WIFI_AUTH_WPA_WPA2_PSK;
                    }
                }
            }

            offset += 2 + ieLen;
        }

        // Apply PMF classification on top of RSN detection
        if (hasRSN && net.authmode == WIFI_AUTH_WPA2_PSK) {
            if (pmf.required) {
                net.authmode = WIFI_AUTH_WPA3_PSK;         // MFPR=1: pure WPA3-SAE
            } else if (pmf.capable) {
                net.authmode = WIFI_AUTH_WPA2_WPA3_PSK;    // MFPC=1 only: transitional
            }
        }
        
        if (net.channel == 0) {
            net.channel = currentChannel;
        }
        
        // Queue for deferred add
        enqueuePendingNetwork(net);
    } else {
        // Update existing network
        enterCritical();
        if (idx >= 0 && idx < (int)networks.size()) {
            DetectedNetwork& net = networks[idx];
            net.rssi = rssi;
            net.rssiAvg = updateRssiAvg(net.rssiAvg, rssi);
            net.lastSeen = now;
            net.beaconCount++;
            if (net.lastBeaconSeen > 0) {
                uint32_t delta = now - net.lastBeaconSeen;
                if (delta > 0 && delta < BEACON_INTERVAL_MAX_MS) {
                    if (net.beaconIntervalEmaMs == 0) {
                        net.beaconIntervalEmaMs = (uint16_t)delta;
                    } else {
                        uint32_t blended = (uint32_t)net.beaconIntervalEmaMs * 7 + delta;
                        net.beaconIntervalEmaMs = (uint16_t)(blended / 8);
                    }
                }
            }
            net.lastBeaconSeen = now;
            net.hasPMF |= pmf.required;
        }
        exitCritical();
    }
}

static void processProbeResponse(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    uint32_t now = millis();

    // Parse SSID from IE (probe responses can reveal hidden SSIDs)
    char ssidBuf[33] = {0};
    bool ssidFound = false;
    bool ssidAllNull = true;
    uint16_t offset = 36;
    while (offset + 2 < len) {
        uint8_t id = payload[offset];
        uint8_t ieLen = payload[offset + 1];
        
        if (offset + 2 + ieLen > len) break;
        
        if (id == 0 && ieLen > 0 && ieLen <= 32) {
            memcpy(ssidBuf, payload + offset + 2, ieLen);
            ssidBuf[ieLen] = 0;
            ssidFound = true;
            
            for (uint8_t i = 0; i < ieLen; i++) {
                if (ssidBuf[i] != 0) { ssidAllNull = false; break; }
            }
            break;
        }
        
        offset += 2 + ieLen;
    }
    
    // [BUG5 FIX] Do lookup inside critical section to prevent TOCTOU race
    // cleanupStaleNetworks() can modify vector between lookup and use
    enterCritical();
    int idx = findNetworkInternal(bssid);
    
    if (idx < 0) {
        exitCritical();
        if (ssidFound && !ssidAllNull) {
            revealSsidIfKnown(bssid, ssidBuf);
        }
        return;
    }
    
    // idx is valid and we hold the lock - safe to use
    if ((networks[idx].ssid[0] == 0 || networks[idx].isHidden) && ssidFound && !ssidAllNull) {
        memcpy(networks[idx].ssid, ssidBuf, 33);
        networks[idx].isHidden = false;
    }
    
    networks[idx].rssi = rssi;
    networks[idx].rssiAvg = updateRssiAvg(networks[idx].rssiAvg, rssi);
    networks[idx].lastSeen = now;
    exitCritical();
}

static void processAssocRequest(const uint8_t* payload, uint16_t len, bool isReassoc) {
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    uint16_t fixedLen = isReassoc ? 10 : 4;
    uint16_t offset = 24 + fixedLen;
    if (offset + 2 > len) return;
    
    // Parse SSID IE from tagged parameters
    char ssidBuf[33] = {0};
    bool ssidFound = false;
    bool ssidAllNull = true;
    while (offset + 2 < len) {
        uint8_t id = payload[offset];
        uint8_t ieLen = payload[offset + 1];
        if (offset + 2 + ieLen > len) break;
        
        if (id == 0 && ieLen > 0 && ieLen <= 32) {
            memcpy(ssidBuf, payload + offset + 2, ieLen);
            ssidBuf[ieLen] = 0;
            ssidFound = true;
            for (uint8_t i = 0; i < ieLen; i++) {
                if (ssidBuf[i] != 0) { ssidAllNull = false; break; }
            }
            break;
        }
        offset += 2 + ieLen;
    }
    
    if (ssidFound && !ssidAllNull) {
        revealSsidIfKnown(bssid, ssidBuf);
    }
}

static void markDataActivity(const uint8_t* bssid, const uint8_t* clientMac) {
    enterCritical();

    int idx = findNetworkInternal(bssid);
    if (idx >= 0 && idx < (int)networks.size()) {
        DetectedNetwork& net = networks[idx];
        net.lastDataSeen = millis();
        if (clientMac) {
            uint8_t bit = clientHashIndex(clientMac);
            if (bit < 64) {
                net.clientBitset |= (1ULL << bit);
            } else {
                net.clientBitsetHigh |= (1ULL << (bit - 64));
            }
        }
    }

    exitCritical();
}

static void processDataFrame(const uint8_t* payload, uint16_t len, int8_t rssi) {
    (void)rssi;
    if (len < 28) return;
    
    uint8_t toDs = (payload[1] & 0x01);
    uint8_t fromDs = (payload[1] & 0x02) >> 1;
    
    const uint8_t* bssid = nullptr;
    const uint8_t* clientMac = nullptr;
    
    if (!toDs && fromDs) {
        bssid = payload + 10;
        clientMac = payload + 4;
    } else if (toDs && !fromDs) {
        bssid = payload + 4;
        clientMac = payload + 10;
    }
    
    if (bssid && clientMac) {
        if ((clientMac[0] & 0x01) == 0) {
            markDataActivity(bssid, clientMac);
        }
    }
}

static void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!buf) return;
    if (!running || paused) return;
    if (busy) {
        PacketCallback cb = modeCallback.load(std::memory_order_relaxed);
        if (cb) {
            cb((wifi_promiscuous_pkt_t*)buf, type);
        }
        return;
    }
    
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;
    
    // ESP32 adds 4 ghost bytes
    if (len > 4) len -= 4;
    if (len < 24) return;
    
    packetCount.fetch_add(1, std::memory_order_relaxed);
    s_core0PktCount.fetch_add(1, std::memory_order_relaxed);
    
    const uint8_t* payload = pkt->payload;
    uint8_t frameSubtype = (payload[0] >> 4) & 0x0F;
    
    // CYD pattern: when a mode callback is active, skip ALL NetworkRecon processing.
    // The mode handles its own beacon/EAPOL/data processing.
    // This makes the callback path spinlock-FREE on core 0, preventing TG1WDT.
    // CYD: "if (_modeCb) { if (type==WIFI_PKT_DATA) cb(); return; }"
    PacketCallback cb = modeCallback.load(std::memory_order_relaxed);
    if (cb) {
        cb(pkt, type);
        return;
    }
    
    // No mode active: process beacons/probes for network discovery (idle/menu)
    if (type == WIFI_PKT_MGMT) {
        if (frameSubtype == 0x08) {  // Beacon
            processBeacon(payload, len, rssi);
        } else if (frameSubtype == 0x05) {  // Probe Response
            processProbeResponse(payload, len, rssi);
        } else if (frameSubtype == 0x00) {  // Assoc Request
            processAssocRequest(payload, len, false);
        } else if (frameSubtype == 0x02) {  // Reassoc Request
            processAssocRequest(payload, len, true);
        }
    } else if (type == WIFI_PKT_DATA) {
        processDataFrame(payload, len, rssi);
    }
}

static void processDeferredEvents() {
    const uint8_t kMaxAddsPerUpdate = 8;
    uint8_t processed = 0;
    DetectedNetwork pending = {};
    
    while (processed < kMaxAddsPerUpdate && dequeuePendingNetwork(pending)) {
        // Apply any deferred SSID reveal before adding
        applyPendingSsid(pending);
        
        // With reserve(MAX_RECON_NETWORKS) at init and no shrink_to_fit,
        // push_back never allocates. At capacity, evict weakest instead of growing.
        bool hasCapacity = networks.size() < networks.capacity();

        bool inserted = false;
        bool replaced = false;
        if (hasCapacity) {
            enterCritical();
            networks.push_back(pending);  // Safe: capacity pre-reserved at init
            exitCritical();
            inserted = true;
        } else {
            // Vector is full - evict a low-value entry if the new one is better
            uint32_t now = millis();
            int pendingScore = computeRetentionScore(pending, now);
            int worstScore = 100000;
            int worstIdx = -1;
            
            enterCritical();
            for (size_t i = 0; i < networks.size(); i++) {
                if (networks[i].isTarget) continue;
                int score = computeRetentionScore(networks[i], now);
                if (score < worstScore) {
                    worstScore = score;
                    worstIdx = (int)i;
                }
            }
            if (worstIdx >= 0 && pendingScore > worstScore) {
                networks[worstIdx] = pending;
                replaced = true;
            }
            exitCritical();
        }
        
        if (inserted || replaced) {
            // Populate SSID cache so handshakes can find SSID after networks[] cleanup
            if (pending.ssid[0] != 0) {
                updateSsidCache(pending.bssid, pending.ssid);
            }
            // Notify mode of new network discovery (for XP events)
            // Called OUTSIDE critical section - safe for Mood/XP calls
            if (newNetworkCallback) {
                newNetworkCallback(
                    pending.authmode,
                    pending.isHidden,
                    pending.ssid,
                    pending.rssi,
                    pending.channel
                );
            }
        }
        
        processed++;
    }
}

static void cleanupStaleNetworks() {
    uint32_t now = millis();
    
    // Phase 1: Collect stale indices WITHOUT holding spinlock.
    // This is the expensive part (iterates all networks). By not holding the lock,
    // the promiscuous callback on core 0 is never blocked here — prevents TG1WDT.
    // Tradeoff: data may change between collect and erase. Phase 2 re-validates.
    static size_t staleIndices[50];
    static uint8_t staleBssid[50][6];  // Snapshot BSSIDs for re-validation
    size_t staleCount = 0;

    for (size_t i = 0; i < networks.size() && staleCount < 50; i++) {
        if (networks[i].lastDataSeen > 0 &&
            now - networks[i].lastDataSeen > CLIENT_BITMAP_RESET_MS) {
            networks[i].clientBitset = 0;
            networks[i].clientBitsetHigh = 0;
        }
        int8_t rssi = (networks[i].rssiAvg != 0) ? networks[i].rssiAvg : networks[i].rssi;
        uint32_t timeout = STALE_TIMEOUT_MS;  // 60s default (strong signal)
        if (rssi < -75) timeout = 120000;     // Weak: 2 min
        else if (rssi < -50) timeout = 90000; // Medium: 1.5 min
        if (now - networks[i].lastSeen > timeout) {
            staleIndices[staleCount] = i;
            memcpy(staleBssid[staleCount], networks[i].bssid, 6);
            staleCount++;
        }
    }
    
    if (staleCount == 0) return;  // Nothing to erase — skip spinlock entirely
    
    // Phase 2: Re-validate under spinlock (minimal hold time — just erase).
    // Indices may have shifted if callback added/removed networks between phases.
    // Match by BSSID to ensure we erase the correct entries.
    enterCritical();
    for (int i = staleCount - 1; i >= 0; i--) {
        // Find current index by BSSID (vector may have shifted)
        size_t idx = staleIndices[i];
        if (idx < networks.size() && memcmp(networks[idx].bssid, staleBssid[i], 6) == 0) {
            networks.erase(networks.begin() + idx);
        }
    }
    exitCritical();
}

// ============================================================================
// Public API Implementation
// ============================================================================

void init() {
    if (initialized) return;
    
    Serial.println("[RECON] Initializing NetworkRecon service...");
    
    networks.clear();
    networks.reserve(MAX_RECON_NETWORKS);  // Full upfront reserve — eliminates growth reallocations
    
    packetCount.store(0, std::memory_order_relaxed);
    currentChannel = 1;
    currentChannelIndex = 0;
    lastHopTime = 0;
    lastCleanupTime = 0;
    running = false;
    paused = false;
    channelLocked.store(false, std::memory_order_relaxed);
    busy = false;
    pendingNetWrite = 0;
    pendingNetRead = 0;
    pendingSsidWrite = 0;
    for (uint8_t i = 0; i < PENDING_SSID_SLOTS; i++) {
        pendingSsids[i].ready.store(false, std::memory_order_relaxed);
    }
    modeCallback.store(nullptr, std::memory_order_relaxed);
    heapStabilized = false;
    
    initialized = true;
    Serial.println("[RECON] Initialized");
}

void start() {
    if (!initialized) init();

    // Re-reserve if freeNetworks() dropped capacity to 0
    if (networks.capacity() == 0) {
        networks.reserve(MAX_RECON_NETWORKS);
    }

    if (running) {
        if (paused) {
            resume();
        }
        return;
    }

    // Allocate SSID cache from internal RAM (PSRAM access causes TG1WDT
    // during dual-core operation — WiFi promiscuous callback on Core 0
    // contends with main loop on Core 1).
    if (!s_ssidCache) {
        s_ssidCache = (SsidCacheEntry*)heap_caps_calloc(SSID_CACHE_SIZE, sizeof(SsidCacheEntry), MALLOC_CAP_INTERNAL);
    }

    Serial.printf("[RECON] Starting background scan... free=%u\r\n",
                  ESP.getFreeHeap());
    
    heapStabilized = false;
    startTime = millis();
    pendingNetWrite = 0;
    pendingNetRead = 0;
    pendingSsidWrite = 0;
    for (uint8_t i = 0; i < PENDING_SSID_SLOTS; i++) {
        pendingSsids[i].ready.store(false, std::memory_order_relaxed);
    }
    
    // Handle BLE coexistence
    if (NimBLEDevice::isInitialized()) {
        Serial.println("[RECON] BLE active - deinitializing for WiFi coexistence");
        
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (pScan && pScan->isScanning()) {
            pScan->stop();
            delay(50);
        }
        
        NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
        if (pAdv && pAdv->isAdvertising()) {
            pAdv->stop();
            delay(50);
        }
        
        NimBLEDevice::deinit(true);
        delay(100);
        yield();  // Let deferred FreeRTOS cleanup tasks coalesce freed BLE memory
        delay(50);

        Serial.printf("[RECON] After BLE deinit: free=%u\r\n",
                      ESP.getFreeHeap());
    }

    // Initialize WiFi
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    delay(50);
    
    // Randomize MAC if configured
    if (Config::wifi().randomizeMAC) {
        WSLBypasser::randomizeMAC();
    }
    
    // DIAG TEST: was WiFi.disconnect() (default args, eraseap=true). That calls
    // esp_wifi_set_config() which may reset the dynamic TX buffer pool. Use
    // disconnect(false, false) to skip the set_config call and preserve pool.
    WiFi.disconnect(false /* wifioff */, false /* eraseap */);
    delay(50);
    
    // Set up promiscuous mode
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
    esp_wifi_set_promiscuous_filter(nullptr);  // Receive all packet types
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    
    running = true;
    paused = false;
    lastHopTime = millis();
    lastCleanupTime = millis();
    
    Serial.printf("[RECON] Started on channel %d\r\n", currentChannel);
}

void stop() {
    if (!running) return;
    
    Serial.println("[RECON] Stopping...");
    
    running = false;
    paused = false;
    manualChannelLock = false;
    manualLockedChannel = 0;
    
    WiFiUtils::stopPromiscuous();
    
    // Free SSID cache (internal RAM, allocated in start())
    if (s_ssidCache) {
        free(s_ssidCache);
        s_ssidCache = nullptr;
    }
    
    // Don't clear networks - they persist for mode reuse
    
    Serial.printf("[RECON] Stopped. Networks cached: %d\r\n", networks.size());
}

void freeNetworks() {
    enterCritical();
    networks.clear();
    networks.shrink_to_fit();
    exitCritical();
    Serial.println("[RECON] Networks vector freed");
}

void pause() {
    if (!running || paused) return;
    
    Serial.println("[RECON] Pausing promiscuous mode...");
    
    // [BUG4 FIX] Save and clear channel lock - will restore on resume if mode still active
    channelLockedBeforePause = channelLocked.load(std::memory_order_acquire);
    if (channelLockedBeforePause) {
        channelLocked.store(false, std::memory_order_release);
        Serial.println("[RECON] Channel lock suspended for pause");
    }
    
    // Disable promiscuous but keep WiFi STA active
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    
    paused = true;  // Set AFTER disabling promiscuous to prevent Core 0 callback race
    
    Serial.println("[RECON] Paused (WiFi STA still active)");
}

void resume() {
    if (!running || !paused) return;
    
    Serial.println("[RECON] Resuming promiscuous mode...");
    
    // Disconnect without resetting TX buffer pool or powering off WiFi.
    // Default WiFi.disconnect() uses eraseap=true which calls esp_wifi_set_config()
    // and can nuke the dynamic TX buffer pool — see AGENTS.md / docs/err257_wdt_fix_log.md.
    WiFi.disconnect(false /* wifioff */, false /* eraseap */);
    delay(50);
    
    // Re-enable promiscuous
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
    esp_wifi_set_promiscuous_filter(nullptr);  // Receive all packet types
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    
    paused = false;
    lastHopTime = millis();
    
    if (channelLockedBeforePause && modeCallback.load(std::memory_order_acquire) != nullptr) {
        channelLocked.store(true, std::memory_order_release);
        Serial.printf("[RECON] Channel lock restored to %d\n", lockedChannel);
    }
    channelLockedBeforePause = false;
    
    Serial.printf("[RECON] Resumed on channel %d\r\n", currentChannel);
}

void suspendRxCallback() {
    if (!running) return;
    // Lightweight: just clear the WiFi-level callback pointer.
    // Does NOT call esp_wifi_set_promiscuous(false) — that blocks the WiFi
    // driver on Core 0 → TG1WDT when called from autosaveTask (same core).
    // WiFi task keeps running (TG1 ISR alive) but no callbacks fire.
    esp_wifi_set_promiscuous_rx_cb(nullptr);
}

void restoreRxCallback() {
    if (!running) return;
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
}

void update() {
    if (!running || paused) return;
    
    // CYD pattern: gate callback to prevent NetworkRecon's own beacon processing
    // from calling enterCritical() on Core 0 while Core 1 accesses networks[].
    // Without this, Core 0's beacon processing can deadlock against Core 1's
    // enterCritical() → TG1WDT_SYS_RST. Mode callbacks handle their own gating.
    busy.store(true, std::memory_order_release);
    
    uint32_t now = millis();
    
    // Process deferred events from callback
    processDeferredEvents();
    
    // Channel hopping
    uint32_t hopInterval = getHopIntervalMsInternal();
    if (!channelLocked.load(std::memory_order_acquire) && now - lastHopTime > hopInterval) {
        hopChannel();
        lastHopTime = now;
    }
    
    // Periodic cleanup
    if (now - lastCleanupTime > CLEANUP_INTERVAL_MS) {
        cleanupStaleNetworks();
        lastCleanupTime = now;

        // NOTE: No shrink_to_fit here. The reserve(MAX_RECON_NETWORKS) at init
        // is permanent during operation. Shrinking would force TLSF to find a
        // new differently-sized block on next growth, creating fragmentation.
        // Capacity is only released in freeNetworks() on mode exit.
    }
    
    // Check heap stabilization
    if (!heapStabilized) {
        size_t currentFree = ESP.getFreeHeap();
        if (currentFree > HeapPolicy::kHeapStableThreshold) {
            heapStabilized = true;
            Serial.printf("[RECON] Heap stabilized in %ums: free=%u\r\n",
                          now - startTime, currentFree);
        }
    }
    
    busy.store(false, std::memory_order_release);
}

bool isRunning() {
    return running && !paused;
}

bool isPaused() {
    return running && paused;
}

bool canSendFramesNow() {
    if (!running) return false;
    if (paused) return false;
    return true;
}

bool isHeapStable() {
    return heapStabilized;
}

uint8_t getCurrentChannel() {
    return currentChannel;
}

uint32_t getHopIntervalMs() {
    return getHopIntervalMsInternal();
}

void setHopIntervalOverride(uint32_t intervalMs) {
    if (intervalMs == 0) {
        hopIntervalOverrideMs.store(0);
        return;
    }
    if (intervalMs < 50) intervalMs = 50;
    if (intervalMs > 2000) intervalMs = 2000;
    hopIntervalOverrideMs.store(intervalMs);
}

void clearHopIntervalOverride() {
    hopIntervalOverrideMs.store(0);
}

uint32_t getPacketCount() {
    return packetCount.load(std::memory_order_relaxed);
}

uint32_t getCore0PacketCount() {
    return s_core0PktCount.load(std::memory_order_relaxed);
}

uint8_t estimateClientCount(const DetectedNetwork& net) {
    return (uint8_t)(__builtin_popcountll(net.clientBitset) +
                     __builtin_popcountll(net.clientBitsetHigh));
}

static inline uint8_t scoreRssi(int8_t rssi) {
    if (rssi <= -95) return 0;
    if (rssi >= -30) return 60;
    return (uint8_t)(((int)rssi + 95) * 60 / 65);
}

static inline uint8_t scoreRecency(uint32_t ageMs) {
    if (ageMs <= 2000) return 20;
    if (ageMs <= 5000) return 12;
    if (ageMs <= 15000) return 5;
    return 0;
}

static inline uint8_t scoreActivity(uint32_t ageMs) {
    if (ageMs <= 3000) return 20;
    if (ageMs <= 10000) return 10;
    if (ageMs <= 30000) return 5;
    return 0;
}

static inline uint8_t scoreBeaconStability(uint16_t intervalEmaMs) {
    if (intervalEmaMs == 0) return 0;
    if (intervalEmaMs <= 150) return 10;
    if (intervalEmaMs <= 500) return 6;
    if (intervalEmaMs <= 1000) return 3;
    return 0;
}

uint8_t getQualityScore(const DetectedNetwork& net) {
    uint32_t now = millis();
    int8_t rssi = (net.rssiAvg != 0) ? net.rssiAvg : net.rssi;
    uint32_t age = now - net.lastSeen;
    uint32_t dataAge = (net.lastDataSeen > 0) ? (now - net.lastDataSeen) : 0xFFFFFFFFu;
    uint8_t score = 0;
    score += scoreRssi(rssi);
    score += scoreRecency(age);
    if (dataAge != 0xFFFFFFFFu) {
        score += scoreActivity(dataAge);
    }
    score += scoreBeaconStability(net.beaconIntervalEmaMs);
    if (score > 100) score = 100;
    return score;
}

std::vector<DetectedNetwork>& getNetworks() {
    return networks;
}

uint16_t getNetworkCount() {
    return networks.size();
}

bool findNetwork(const uint8_t* bssid, DetectedNetwork* out) {
    enterCritical();
    int idx = findNetworkInternal(bssid);
    bool found = (idx >= 0 && idx < (int)networks.size());
    if (found && out) {
        // Copy to caller's buffer while holding lock - pointer becomes invalid after unlock
        *out = networks[idx];
    }
    exitCritical();
    return found;
}

int findNetworkIndex(const uint8_t* bssid) {
    enterCritical();
    int idx = findNetworkInternal(bssid);
    exitCritical();
    return idx;
}

void lockChannel(uint8_t channel) {
    if (manualChannelLock) return;  // User lock has priority
    if (channel < 1 || channel > 14) return;

    lockedChannel = channel;
    currentChannel = channel;
    channelLocked.store(true, std::memory_order_release);
    const uint32_t t0 = (uint32_t)esp_timer_get_time();
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    const uint32_t dt = (uint32_t)esp_timer_get_time() - t0;
    Serial.printf("[RECON] Channel locked to %d (set_channel err=%d dt=%uus)\r\n",
                  channel, (int)err, (unsigned)dt);
}

void unlockChannel() {
    if (manualChannelLock) return;  // User lock has priority
    channelLocked.store(false, std::memory_order_release);
    Serial.println("[RECON] Channel unlocked, resuming hopping");
}

bool isChannelLocked() {
    if (manualChannelLock) return true;
    return channelLocked.load(std::memory_order_acquire);
}

void setChannel(uint8_t channel) {
    if (channel < 1 || channel > 14) return;
    currentChannel = channel;
    const uint32_t t0 = (uint32_t)esp_timer_get_time();
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    const uint32_t dt = (uint32_t)esp_timer_get_time() - t0;
    Serial.printf("[RECON] setChannel ch=%d err=%d dt=%uus\r\n",
                  channel, (int)err, (unsigned)dt);
}

void setManualChannelLock(uint8_t channel) {
    if (channel < 1 || channel > 13) return;
    manualChannelLock = true;
    manualLockedChannel = channel;
    currentChannel = channel;
    channelLocked.store(true, std::memory_order_release);
    const uint32_t t0 = (uint32_t)esp_timer_get_time();
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    const uint32_t dt = (uint32_t)esp_timer_get_time() - t0;
    Serial.printf("[RECON] Manual channel lock: CH %d (set_channel err=%d dt=%uus)\r\n",
                  channel, (int)err, (unsigned)dt);
}

void clearManualChannelLock() {
    if (!manualChannelLock) return;
    manualChannelLock = false;
    manualLockedChannel = 0;
    channelLocked.store(false, std::memory_order_release);
    Serial.println("[RECON] Manual channel lock cleared, resuming hopping");
}

bool isManualChannelLocked() {
    return manualChannelLock;
}

uint8_t getManualLockedChannel() {
    return manualLockedChannel;
}

void setPacketCallback(PacketCallback callback) {
    modeCallback.store(callback, std::memory_order_release);
}

void setNewNetworkCallback(NewNetworkCallback callback) {
    newNetworkCallback = callback;
}

void enterCritical() {
#ifdef NETRECON_LOCK_PROFILE
    // Record enter timestamp BEFORE taking the lock. esp_timer_get_time
    // is a single register read on Xtensa, no syscall.
    s_lockProfileEnterUs.store((uint32_t)(esp_timer_get_time() & 0xFFFFFFFF),
                               std::memory_order_relaxed);
    s_lockProfileInLock.store(1, std::memory_order_relaxed);
#endif
    taskENTER_CRITICAL(&vectorMux);
}

void exitCritical() {
    taskEXIT_CRITICAL(&vectorMux);
#ifdef NETRECON_LOCK_PROFILE
    if (s_lockProfileInLock.load(std::memory_order_relaxed)) {
        const uint32_t enterUs = s_lockProfileEnterUs.load(std::memory_order_relaxed);
        const uint32_t exitUs  = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF);
        // Handle microsecond wrap (esp_timer_get_time is 64-bit, we cast to
        // 32-bit so wrap happens at ~71 minutes). Unlikely to matter for
        // per-tick lock holds (always < 100ms).
        uint32_t holdUs = (exitUs >= enterUs) ? (exitUs - enterUs) : 0;
        // Atomic fetch-add for the running total, atomic load+max for the peak.
        uint32_t prevMax = s_lockProfileMaxUs.load(std::memory_order_relaxed);
        while (holdUs > prevMax &&
               !s_lockProfileMaxUs.compare_exchange_weak(prevMax, holdUs,
                                                         std::memory_order_relaxed)) {
            // prevMax was updated by another writer; retry
        }
        s_lockProfileTotalUs.fetch_add(holdUs, std::memory_order_relaxed);
        s_lockProfileCallCount.fetch_add(1, std::memory_order_relaxed);
        s_lockProfileInLock.store(0, std::memory_order_relaxed);
    }
#endif
}

// ============================================================================
// SSID Cache
// ============================================================================

void updateSsidCache(const uint8_t* bssid, const char* ssid) {
    if (!s_ssidCache || !bssid || !ssid || ssid[0] == 0) return;
    uint32_t now = millis();
    int oldestIdx = 0;
    uint32_t oldestTime = UINT32_MAX;
    for (int i = 0; i < SSID_CACHE_SIZE; i++) {
        if (memcmp(s_ssidCache[i].bssid, bssid, 6) == 0) {
            strncpy(s_ssidCache[i].ssid, ssid, 32);
            s_ssidCache[i].ssid[32] = 0;
            s_ssidCache[i].lastSeen = now;
            return;
        }
        if (s_ssidCache[i].lastSeen < oldestTime) {
            oldestTime = s_ssidCache[i].lastSeen;
            oldestIdx = i;
        }
    }
    memcpy(s_ssidCache[oldestIdx].bssid, bssid, 6);
    strncpy(s_ssidCache[oldestIdx].ssid, ssid, 32);
    s_ssidCache[oldestIdx].ssid[32] = 0;
    s_ssidCache[oldestIdx].lastSeen = now;
}

bool lookupSsidCache(const uint8_t* bssid, char* ssidOut, size_t maxLen) {
    if (!s_ssidCache || !bssid || !ssidOut || maxLen == 0) return false;
    for (int i = 0; i < SSID_CACHE_SIZE; i++) {
        if (s_ssidCache[i].lastSeen > 0 && memcmp(s_ssidCache[i].bssid, bssid, 6) == 0) {
            strncpy(ssidOut, s_ssidCache[i].ssid, maxLen - 1);
            ssidOut[maxLen - 1] = 0;
            return true;
        }
    }
    return false;
}

} // namespace NetworkRecon
