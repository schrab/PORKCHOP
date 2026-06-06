/**
 * PigSync ESP-NOW Client Implementation (Porkchop/POPS side)
 * 
 * SON OF A PIG - Reliable sync with Sirloin devices
 */

#include "pigsync_client.h"
#include "pigsync_protocol.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <SD.h>
#include "../hal/hal_input.h"
#include <sys/time.h>  // Phase 3: settimeofday for RTC sync
#include "../core/config.h"
#include "../core/sdlog.h"
#include "../core/sd_layout.h"
#include "../core/wifi_utils.h"
#include "../core/heap_gates.h"
#include "../core/heap_policy.h"
#include "../core/network_recon.h"
#include "../piglet/mood.h"
#include "../ui/display.h"
#include "../modes/warhog.h"
#include "../modes/oink.h"
#include "../web/wpasec.h"
#include "../web/wigle.h"


#ifndef PIGSYNC_LOG_ENABLED
#define PIGSYNC_LOG_ENABLED 0
#endif

#if PIGSYNC_LOG_ENABLED
#define PIGSYNC_LOGF(...) Serial.printf(__VA_ARGS__)
#define PIGSYNC_LOGLN(msg) Serial.println(msg)
#define PIGSYNC_LOG(msg) Serial.print(msg)
#else
#define PIGSYNC_LOGF(...) do {} while (0)
#define PIGSYNC_LOGLN(...) do {} while (0)
#define PIGSYNC_LOG(msg) do {} while (0)
#endif

// ==[ STATIC MEMBER DEFINITIONS ]==
bool PigSyncMode::running = false;
bool PigSyncMode::initialized = false;
uint8_t PigSyncMode::selectedIndex = 0;

std::vector<SirloinDevice> PigSyncMode::devices;
uint8_t PigSyncMode::connectedMac[6] = {0};
bool PigSyncMode::connected = false;

uint16_t PigSyncMode::remotePMKIDCount = 0;
uint16_t PigSyncMode::remoteHSCount = 0;
uint16_t PigSyncMode::totalSynced = 0;
uint16_t PigSyncMode::syncedPMKIDs = 0;
uint16_t PigSyncMode::syncedHandshakes = 0;

PigSyncMode::State PigSyncMode::state = PigSyncMode::State::IDLE;

uint8_t PigSyncMode::currentType = 0;
uint16_t PigSyncMode::currentIndex = 0;
uint16_t PigSyncMode::totalChunks = 0;
uint16_t PigSyncMode::receivedChunks = 0;

SyncProgress PigSyncMode::progress = {0};
uint8_t PigSyncMode::rxBuffer[2048] = {0};
uint16_t PigSyncMode::rxBufferLen = 0;
char PigSyncMode::lastError[64] = {0};

uint8_t PigSyncMode::dialogueId = 0;
uint8_t PigSyncMode::dialoguePhase = 0;
uint32_t PigSyncMode::callStartTime = 0;
uint32_t PigSyncMode::phraseStartTime = 0;
char PigSyncMode::papaGoodbyeSelected[64] = {0};

uint32_t PigSyncMode::lastDiscoveryTime = 0;
uint32_t PigSyncMode::discoveryStartTime = 0;
bool PigSyncMode::scanning = false;
uint32_t PigSyncMode::connectStartTime = 0;
uint32_t PigSyncMode::lastHelloTime = 0;
uint8_t PigSyncMode::helloRetryCount = 0;
uint32_t PigSyncMode::readyStartTime = 0;
uint8_t PigSyncMode::channelRetryCount = 0;
uint32_t PigSyncMode::syncCompleteTime = 0;

uint16_t PigSyncMode::sessionId = 0;
uint8_t PigSyncMode::remoteMood = 128;
uint8_t PigSyncMode::lastBountyMatches = 0;
uint8_t PigSyncMode::dataChannel = PIGSYNC_DISCOVERY_CHANNEL;

// Reliability tracking
static PigSyncReliability reliability;

PigSyncMode::CaptureCallback PigSyncMode::onCaptureCb = nullptr;
PigSyncMode::SyncCompleteCallback PigSyncMode::onSyncCompleteCb = nullptr;

// Peer info for connected Sirloin
static esp_now_peer_info_t sirloinPeer;

// ==[ CONTROL TX RELIABILITY ]==
static const size_t CONTROL_TX_MAX = 160;
static const uint8_t CONTROL_QUEUE_MAX = 3;
struct ControlTxState {
    bool waiting;
    uint8_t type;
    uint8_t seq;
    uint32_t lastSend;
    uint8_t retries;
    size_t len;
    uint8_t mac[6];
    uint8_t buf[CONTROL_TX_MAX];
};

static ControlTxState controlTx = {};
static ControlTxState controlQueue[CONTROL_QUEUE_MAX] = {};
static uint8_t controlQueueHead = 0;
static uint8_t controlQueueTail = 0;
static uint8_t controlQueueCount = 0;
static bool pendingStartSync = false;
static bool pendingNextCapture = false;

static bool isControlCommand(uint8_t type) {
    switch (type) {
        case CMD_HELLO:
        case CMD_READY:
        case CMD_GET_COUNT:
        case CMD_MARK_SYNCED:
        case CMD_PURGE:
        case CMD_BOUNTIES:
        case CMD_TIME_SYNC:
            return true;
        default:
            return false;
    }
}

static bool isControlResponse(uint8_t type) {
    switch (type) {
        case RSP_RING:
        case RSP_HELLO:
        case RSP_READY:
        case RSP_COUNT:
        case RSP_OK:
        case RSP_ERROR:
        case RSP_PURGED:
        case RSP_BOUNTIES_ACK:
        case RSP_TIME_SYNC:
        case RSP_DISCONNECT:
            return true;
        default:
            return false;
    }
}

static bool isSessionBoundResponse(uint8_t type) {
    switch (type) {
        case RSP_READY:
        case RSP_OK:
        case RSP_ERROR:
        case RSP_DISCONNECT:
        case RSP_COUNT:
        case RSP_CHUNK:
        case RSP_COMPLETE:
        case RSP_PURGED:
        case RSP_BOUNTIES_ACK:
        case RSP_TIME_SYNC:
            return true;
        default:
            return false;
    }
}

static void enqueueControl(const uint8_t* mac, const uint8_t* buf, size_t len, uint8_t type, uint8_t seq) {
    if (controlQueueCount >= CONTROL_QUEUE_MAX) {
        return;
    }
    ControlTxState& slot = controlQueue[controlQueueTail];
    memcpy(slot.buf, buf, len);
    slot.len = len;
    slot.type = type;
    slot.seq = seq;
    memcpy(slot.mac, mac, sizeof(slot.mac));
    slot.retries = 0;
    slot.waiting = true;
    slot.lastSend = 0;
    controlQueueTail = (controlQueueTail + 1) % CONTROL_QUEUE_MAX;
    controlQueueCount++;
}

static bool dequeueControl(ControlTxState& out) {
    if (controlQueueCount == 0) {
        return false;
    }
    out = controlQueue[controlQueueHead];
    controlQueueHead = (controlQueueHead + 1) % CONTROL_QUEUE_MAX;
    controlQueueCount--;
    return true;
}

static void resetControlQueue() {
    controlQueueHead = 0;
    controlQueueTail = 0;
    controlQueueCount = 0;
}

static void sendControlPacket(const uint8_t* mac, const uint8_t* buf, size_t len, uint8_t type, uint8_t seq) {
    if (len > CONTROL_TX_MAX) {
        return;
    }
    if (!controlTx.waiting) {
        memcpy(controlTx.buf, buf, len);
        controlTx.len = len;
        controlTx.type = type;
        controlTx.seq = seq;
        memcpy(controlTx.mac, mac, sizeof(controlTx.mac));
        controlTx.retries = 0;
        controlTx.waiting = true;
        controlTx.lastSend = millis();
        esp_now_send(controlTx.mac, controlTx.buf, controlTx.len);
        return;
    }
    enqueueControl(mac, buf, len, type, seq);
}

static void trySendQueuedControl() {
    if (controlTx.waiting) {
        return;
    }
    ControlTxState next = {};
    if (!dequeueControl(next)) {
        return;
    }
    controlTx = next;
    controlTx.lastSend = millis();
    esp_now_send(controlTx.mac, controlTx.buf, controlTx.len);
}

static void clearControlTx() {
    controlTx.waiting = false;
    controlTx.len = 0;
    controlTx.type = 0;
    controlTx.seq = 0;
    controlTx.retries = 0;
    controlTx.lastSend = 0;
    trySendQueuedControl();
}

static bool parseSirloinPMKID(const uint8_t* data, uint16_t len, CapturedPMKID& out) {
    if (!data || len < 65) {
        return false;
    }

    memset(&out, 0, sizeof(out));
    memcpy(out.bssid, data, 6);
    memcpy(out.station, data + 6, 6);

    uint8_t ssidLen = data[12];
    if (ssidLen > 32) ssidLen = 32;
    memcpy(out.ssid, data + 13, ssidLen);
    out.ssid[ssidLen] = '\0';

    memcpy(out.pmkid, data + 45, 16);
    out.timestamp = millis();
    out.saved = false;
    out.saveAttempts = 0;
    return true;
}

static bool parseSirloinHandshake(const uint8_t* data, uint16_t len, CapturedHandshake& out) {
    if (!data || len < 48) {
        return false;
    }

    memset(&out, 0, sizeof(out));
    out.beaconData = nullptr;
    out.beaconLen = 0;

    memcpy(out.bssid, data, 6);
    memcpy(out.station, data + 6, 6);

    uint8_t ssidLen = data[12];
    if (ssidLen > 32) ssidLen = 32;
    memcpy(out.ssid, data + 13, ssidLen);
    out.ssid[ssidLen] = '\0';

    size_t offset = 45;  // bssid(6) + station(6) + ssid_len(1) + ssid(32)
    if (offset + 3 > len) {
        return false;
    }

    // Skip serialized mask (we recompute from parsed frames).
    offset += 1;

    uint16_t beaconLen = data[offset] | (data[offset + 1] << 8);
    offset += 2;

    if (beaconLen > 0) {
        if (beaconLen > 512 || offset + beaconLen > len) {
            return false;
        }
        out.beaconData = (uint8_t*)malloc(beaconLen);
        if (!out.beaconData) {
            return false;
        }
        memcpy(out.beaconData, data + offset, beaconLen);
        out.beaconLen = beaconLen;
        offset += beaconLen;
    }

    out.capturedMask = 0;
    while (offset < len) {
        if (offset + 2 > len) break;
        uint16_t frameLen = data[offset] | (data[offset + 1] << 8);
        offset += 2;
        if (offset + frameLen > len) break;
        const uint8_t* frameData = data + offset;
        offset += frameLen;

        if (offset + 2 > len) break;
        uint16_t fullLen = data[offset] | (data[offset + 1] << 8);
        offset += 2;
        if (offset + fullLen + 6 > len) break;  // msg+rss+ts
        const uint8_t* fullFrame = data + offset;
        offset += fullLen;

        uint8_t msgNum = data[offset++];
        int8_t rssi = (int8_t)data[offset++];
        uint32_t ts = data[offset] |
                      (data[offset + 1] << 8) |
                      (data[offset + 2] << 16) |
                      (data[offset + 3] << 24);
        offset += 4;

        if (msgNum < 1 || msgNum > 4) {
            continue;
        }

        EAPOLFrame& frame = out.frames[msgNum - 1];
        uint16_t copyLen = frameLen;
        if (copyLen > sizeof(frame.data)) copyLen = sizeof(frame.data);
        memcpy(frame.data, frameData, copyLen);
        frame.len = copyLen;

        uint16_t fullCopyLen = fullLen;
        if (fullCopyLen > sizeof(frame.fullFrame)) fullCopyLen = sizeof(frame.fullFrame);
        if (fullCopyLen > 0) {
            memcpy(frame.fullFrame, fullFrame, fullCopyLen);
        }
        frame.fullFrameLen = fullCopyLen;
        frame.messageNum = msgNum;
        frame.rssi = rssi;
        frame.timestamp = (ts < 1000000000) ? ts : millis();

        out.capturedMask |= (1 << (msgNum - 1));
    }

    out.firstSeen = millis();
    out.lastSeen = out.firstSeen;
    out.saved = false;
    out.saveAttempts = 0;

    if (out.capturedMask == 0) {
        if (out.beaconData) {
            free(out.beaconData);
            out.beaconData = nullptr;
        }
        out.beaconLen = 0;
        return false;
    }

    return true;
}

static void removeIfExists(const char* path) {
    if (path && SD.exists(path)) {
        SD.remove(path);
    }
}

static uint8_t getControlMaxRetries(uint8_t type) {
    if (type == CMD_HELLO) {
        uint16_t retries = PIGSYNC_HELLO_TIMEOUT / PIGSYNC_ACK_TIMEOUT;
        if (retries < 1) retries = 1;
        if (retries > 255) retries = 255;
        return (uint8_t)retries;
    }
    if (type == CMD_READY) {
        uint16_t retries = PIGSYNC_READY_TIMEOUT / PIGSYNC_ACK_TIMEOUT;
        if (retries < 1) retries = 1;
        if (retries > 255) retries = 255;
        return (uint8_t)retries;
    }
    return PIGSYNC_MAX_RETRIES;
}

static void handleControlAck(const PigSyncHeader* hdr) {
    if (!controlTx.waiting) return;
    if (hdr->ack == controlTx.seq) {
        clearControlTx();
    }
}

static volatile bool pendingControlAck = false;
static volatile uint8_t pendingControlAckSeq = 0;

// Upgrade peer to encrypted after RSP_HELLO received
static void upgradePeerEncryption(const uint8_t* mac, uint8_t channel) {
    if (mac[0] == 0 && mac[1] == 0 && mac[2] == 0) return;
    
    // Remove and re-add with encryption on specified channel
    esp_now_del_peer(mac);
    
    memset(&sirloinPeer, 0, sizeof(sirloinPeer));
    memcpy(sirloinPeer.peer_addr, mac, 6);
    sirloinPeer.channel = channel;
    sirloinPeer.encrypt = true;
    memcpy(sirloinPeer.lmk, PIGSYNC_LMK, 16);
    
    esp_now_add_peer(&sirloinPeer);
    PIGSYNC_LOGF("[PIGSYNC-CLI-PEER] Upgraded to encrypted on ch%d\n", channel);
}

// Pending data from callbacks (processed in update())
static volatile bool pendingRingReceived = false;
static volatile uint32_t pendingRingAt = 0;
static volatile bool pendingHelloReceived = false;
static volatile bool pendingHelloClearControl = false;
static volatile uint16_t pendingPMKIDCount = 0;
static volatile uint16_t pendingHSCount = 0;
static volatile uint8_t pendingDialogueId = 0;
static volatile uint8_t pendingMood = 128;
static volatile uint16_t pendingSessionId = 0;  // 16-bit session
static volatile uint8_t pendingDataChannel = PIGSYNC_DISCOVERY_CHANNEL;  // Data channel from RSP_HELLO

static volatile bool pendingReadyReceived = false;  // RSP_READY from Sirloin
static volatile bool pendingReadyClearControl = false;

static portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;

// Name reveal notification for UI
static volatile bool pendingNameReveal = false;
static char pendingNameRevealName[16] = {0};

static volatile bool pendingChunkReceived = false;
static const uint8_t PENDING_CHUNK_QUEUE_SIZE = 8;
struct PendingChunkSlot {
    bool used;
    uint16_t seq;
    uint16_t total;
    uint16_t len;
    uint8_t data[256];
};
static PendingChunkSlot pendingChunkQueue[PENDING_CHUNK_QUEUE_SIZE] = {};
static uint8_t pendingChunkCount = 0;
static void clearPendingChunkQueue() {
    taskENTER_CRITICAL(&pendingMux);
    for (uint8_t i = 0; i < PENDING_CHUNK_QUEUE_SIZE; i++) {
        pendingChunkQueue[i].used = false;
    }
    pendingChunkCount = 0;
    pendingChunkReceived = false;
    taskEXIT_CRITICAL(&pendingMux);
}

static volatile bool pendingCompleteReceived = false;
static volatile uint16_t pendingTotalBytes = 0;
static volatile uint32_t pendingCRC = 0;

static volatile bool pendingPurgedReceived = false;
static volatile uint16_t pendingPurgedCount = 0;
static volatile uint8_t pendingBountyMatches = 0;

static volatile bool pendingErrorReceived = false;
static volatile uint8_t pendingErrorCode = 0;

// Forward declarations for pending device updates
static volatile bool pendingBeaconReceived = false;
static uint8_t pendingBeaconMac[6] = {0};
static int8_t pendingBeaconRSSI = 0;
static uint16_t pendingBeaconPending = 0;
static uint8_t pendingBeaconFlags = 0;

// Phase 3: Grunt beacon data
static volatile bool pendingGruntReceived = false;
static uint8_t pendingGruntMac[6] = {0};
static uint8_t pendingGruntFlags = 0;
static uint8_t pendingGruntCaptureCount = 0;
static uint8_t pendingGruntBattery = 0;
static uint8_t pendingGruntStorage = 0;
static uint32_t pendingGruntUnixTime = 0;
static uint16_t pendingGruntUptime = 0;
static char pendingGruntName[5] = {0};

// Phase 3: Time sync response
static volatile bool pendingTimeSyncReceived = false;
static uint8_t pendingTimeSyncValid = 0;
static uint32_t pendingTimeSyncUnix = 0;
static uint32_t pendingTimeSyncRtt = 0;  // Round-trip time in ms

