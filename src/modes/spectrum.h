// HOG ON SPECTRUM Mode - WiFi Spectrum Analyzer
#pragma once

class DisplayCanvas;

#include <Arduino.h>
#include <vector>
#include <atomic>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <FS.h>

// Client monitoring constants
#define MAX_SPECTRUM_CLIENTS 8
#define MAX_SPECTRUM_NETWORKS 64  // Reduced from 100 for cleaner display
#define CLIENT_STALE_TIMEOUT_MS 30000  // 30s before client considered gone
#define VISIBLE_CLIENTS 4              // How many fit on screen
#define SIGNAL_LOST_TIMEOUT_MS 15000   // 15s no beacon = signal lost
#define CLIENT_BEEP_LIMIT 4            // Only beep for first N clients

// Client tracking for monitored network
struct SpectrumClient {
    uint8_t mac[6];
    int8_t rssi;
    uint32_t lastSeen;
    const char* vendor;  // Cached OUI lookup
};

struct SpectrumNetwork {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;         // 1-13
    int8_t rssi;             // Latest RSSI
    uint32_t lastSeen;       // millis() of last beacon
    wifi_auth_mode_t authmode; // Security type (OPEN/WEP/WPA/WPA2/WPA3)
    bool hasPMF;             // Protected Management Frames (immune to deauth)
    bool isHidden;           // Hidden SSID (beacon had empty SSID)
    bool wasRevealed;        // SSID was revealed via probe response
    float displayFreqMHz;    // Smoothed frequency for rendering (prevents left/right jitter)
    // Client tracking (only populated when monitoring THIS network)
    SpectrumClient clients[MAX_SPECTRUM_CLIENTS];
    uint8_t clientCount;
};

// Render snapshot (heap-safe, no vector pointers)
struct SpectrumRenderNet {
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    wifi_auth_mode_t authmode;
    bool hasPMF;
    bool isHidden;
    float displayFreqMHz;
};

struct SpectrumRenderSelected {
    bool valid;
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    wifi_auth_mode_t authmode;
    bool hasPMF;
    bool wasRevealed;
};

struct SpectrumRenderMonitor {
    bool valid;
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    uint8_t clientCount;
    SpectrumClient clients[MAX_SPECTRUM_CLIENTS];
};

// MAC comparison helper [P8]
inline bool macEqual(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}

// Filter modes for target selection
enum class SpectrumFilter : uint8_t {
    ALL = 0,   // Show all networks
    VULN,      // OPEN/WEP/WPA only (weak security)
    SOFT,      // No PMF (deauth-able)
    HIDDEN     // Hidden SSIDs only
};

// PMKID capture for manual attack mode
struct SpectrumPMKID {
    uint8_t bssid[6];
    uint8_t station[6];
    char    ssid[33];
    uint8_t pmkid[16];
    uint32_t timestamp;
    bool    saved;
    uint8_t saveAttempts;  // 0-3, give up after 3
};

// EAPOL frame storage for handshake capture (spectrum-specific, smaller than OINK)
struct SpectrumEAPOLFrame {
    uint8_t data[128];       // EAPOL payload (128 covers M1 PMKID + M2 MIC)
    uint8_t fullFrame[256];  // Full 802.11 frame for PCAP
    uint16_t len;            // EAPOL payload length
    uint16_t fullFrameLen;   // Full 802.11 frame length
    uint8_t messageNum;      // 1-4
    int8_t rssi;
};

// Handshake capture constants
#define SPECTRUM_HS_MAX 2            // Max captured handshakes in attack mode
#define SPECTRUM_HS_PENDING 2        // Circular buffer slots for pending frames
#define SPECTRUM_MAX_BEACON_SIZE 350  // Max beacon frame size for PCAP

// Full 4-way handshake capture (spectrum-specific, uses smaller frame buffers)
struct SpectrumCapturedHandshake {
    uint8_t bssid[6];
    uint8_t station[6];
    char ssid[33];
    SpectrumEAPOLFrame frames[4];    // M1, M2, M3, M4
    uint8_t capturedMask;    // Bits 0-3 for M1-M4
    uint32_t firstSeen;
    uint32_t lastSeen;
    bool saved;
    uint8_t saveAttempts;
    
    bool hasM1() const { return capturedMask & 0x01; }
    bool hasM2() const { return capturedMask & 0x02; }
    bool hasM3() const { return capturedMask & 0x04; }
    bool hasM4() const { return capturedMask & 0x08; }
    bool hasValidPair() const { return (hasM1() && hasM2()) || (hasM2() && hasM3()); }
    bool isComplete() const { return hasValidPair(); }
    uint8_t getMessagePair() const {
        if (hasM1() && hasM2()) return 0x00;
        if (hasM2() && hasM3()) return 0x02;
        return 0xFF;
    }
};

class SpectrumMode {
public:
    static void init();
    static void start();
    static void stop();
    static void update();
    static void draw(DisplayCanvas& canvas);
    static bool isRunning() { return running; }
    
    // For promiscuous callback - updates network RSSI
    static void onBeacon(const uint8_t* bssid, uint8_t channel, bool channelTrusted, int8_t rssi, const char* ssid, wifi_auth_mode_t authmode, bool hasPMF, bool isProbeResponse);
    
    // Bottom bar info
    static void getSelectedInfo(char* out, size_t len);
    
    // Client monitoring accessors [P3]
    static bool isMonitoring() { return monitoringNetwork; }
    static const char* getMonitoredSSID();
    static int getClientCount();
    static uint8_t getMonitoredChannel() { return monitoredChannel; }
    
private:
    static bool running;
    static std::atomic<bool> busy;   // Guard against callback race (atomic for cross-core visibility)
    static std::vector<SpectrumNetwork> networks;
    static SpectrumRenderNet renderNets[MAX_SPECTRUM_NETWORKS];
    static uint16_t renderCount;
    static SpectrumRenderSelected renderSelected;
    static SpectrumRenderMonitor renderMonitor;
    static float viewCenterMHz;      // Center of visible spectrum
    static float viewWidthMHz;       // Visible bandwidth
    static int selectedIndex;        // Currently highlighted network
    static uint32_t lastUpdateTime;
    static bool keyWasPressed;
    static uint8_t currentChannel;   // Current hop channel
    static uint32_t startTime;       // When mode started (for achievement)
    
    // Filter state
    static SpectrumFilter filter;    // Current filter mode
    
    // Deferred logging for revealed SSIDs (avoid Serial in callback)
    static volatile bool pendingReveal;
    static char pendingRevealSSID[33];
    
    // Deferred network add (avoid push_back in callback - ESP32 dual-core race)
    static std::atomic<bool> pendingNetworkAdd;  // Atomic for cross-core visibility (WiFi task → main loop)
    static SpectrumNetwork pendingNetwork;
    
    // Client monitoring state [P1] [P2]
    static bool monitoringNetwork;       // True when locked on network
    static int monitoredNetworkIndex;    // Index of network being monitored
    static uint8_t monitoredBSSID[6];    // [P2] Store BSSID, not just index!
    static uint8_t monitoredChannel;     // Locked channel
    static int clientScrollOffset;       // For scrolling client list
    static int selectedClientIndex;      // Currently highlighted client
    static uint32_t lastClientPrune;     // Last stale client cleanup
    static uint8_t clientsDiscoveredThisSession;  // For limiting beeps
    static volatile bool pendingClientBeep;       // Deferred beep for new client
    static volatile uint8_t pendingNetworkXP;     // Deferred XP for new networks (avoids callback crash)
    
    // Achievement tracking for client monitor (v0.1.6)
    static uint32_t clientMonitorEntryTime;  // When we entered client monitor
    static uint8_t deauthsThisMonitor;       // Deauths since entering monitor
    static uint32_t firstDeauthTime;         // Time of first deauth (for QUICK_DRAW)
    
    // Client detail popup state
    static bool clientDetailActive;          // Detail popup visible
    static uint8_t detailClientMAC[6];       // MAC of client being viewed (close if changes)
    
    // Reveal mode state (broadcast deauth to discover clients)
    static bool revealingClients;            // True when in reveal mode
    static uint32_t revealStartTime;         // When reveal mode started
    static uint32_t lastRevealBurst;         // Last broadcast deauth time
    
    // Attack mode state (manual PMKID capture)
    static bool attackMode;                  // Actively attacking an AP
    static uint8_t attackBSSID[6];           // Target BSSID
    static uint8_t attackChannel;            // Target channel
    static uint32_t attackStartTime;         // When attack began
    static uint32_t lastAttackDeauth;        // Deauth throttle timer
    static uint32_t deauthCount;             // Deauths sent this attack
    static SpectrumPMKID capturedPMKIDs[4];  // Captured PMKIDs (static pool)
    static uint8_t capturedPMKIDCount;       // How many captured
    static volatile bool pendingAttackPMKID; // Flag: PMKID captured (set in callback)
    static volatile bool pendingAttackPMKIDSaved; // Flag: PMKID saved to SD
    // Circular buffer for PMKID from callback to main thread
    static const uint8_t ATTACK_PMKID_SLOTS = 4;
    struct PendingAttackPMKID {
        uint8_t bssid[6];
        uint8_t station[6];
        uint8_t pmkid[16];
        char ssid[33];
    };
    static PendingAttackPMKID attackPMKIDPool[4];
    static volatile uint8_t attackPmkidWrite;
    static volatile uint8_t attackPmkidRead;
    
    // Beacon frame storage for PCAP (shared, attack mode is single-BSSID)
    static uint8_t attackBeaconBuf[SPECTRUM_MAX_BEACON_SIZE];
    static uint16_t attackBeaconLen;
    static volatile bool attackBeaconCaptured;
    
    // Handshake capture state (OINK-compatible full 4-way)
    static SpectrumCapturedHandshake capturedHandshakes[SPECTRUM_HS_MAX];
    static uint8_t capturedHandshakeCount;
    struct PendingHandshakeEntry {
        uint8_t bssid[6];
        uint8_t station[6];
        SpectrumEAPOLFrame frames[4];
        uint8_t capturedMask;
    };
    static PendingHandshakeEntry pendingHandshakePool[SPECTRUM_HS_PENDING];
    static volatile uint8_t pendingHsWrite;
    static volatile uint8_t pendingHsRead;
    
    // Dial mode state (tilt-to-tune when device upright)
    static bool dialMode;                    // Auto-enabled when UPS (upright)
    static bool dialLocked;                  // Channel lock (space toggles)
    static bool dialWasUpright;              // Hysteresis state for FLT/UPS detection
    static uint8_t dialChannel;              // Current dial channel (1-13)
    static float dialPositionTarget;         // Raw gyro position (1.0-13.0)
    static float dialPositionSmooth;         // Lerped display position (smooth)
    static uint32_t lastDialUpdate;          // Timing for lerp
    static uint32_t dialModeEntryTime;       // When dial mode was entered (debounce)
    static volatile uint32_t ppsCounter;     // Packet counter (callback increments)
    static uint32_t displayPps;              // Displayed pps (updated per second)
    static uint32_t lastPpsUpdate;           // Last pps calculation time
    
    static void handleInput();
    static void handleClientMonitorInput();  // Input when monitoring
    static void drawSpectrum(DisplayCanvas& canvas);
    static void drawClientOverlay(DisplayCanvas& canvas);  // Client list overlay
    static void drawClientDetail(DisplayCanvas& canvas);   // Client detail popup
    static void drawGaussianLobe(DisplayCanvas& canvas, float centerFreqMHz, int8_t rssi, bool filled, uint16_t activityPps, uint8_t seed);
    static void drawAxis(DisplayCanvas& canvas);
    static void drawChannelMarkers(DisplayCanvas& canvas);
    static void drawFilterBar(DisplayCanvas& canvas);     // Filter indicator bar
    static void drawDialInfo(DisplayCanvas& canvas);      // Dial mode info bar
    static void drawNoiseFloor(DisplayCanvas& canvas);    // Animated noise at baseline
    static void drawWaterfall(DisplayCanvas& canvas);     // Historical spectrum waterfall
    static void updateSpectrumBuffers();             // Populate buffers from network data
    static void updateWaterfall();                   // Push to waterfall history
    static void pruneStale();            // Remove networks not seen recently
    static void pruneStaleClients();     // Remove clients not seen recently
    static void updateDialChannel();     // Update dial mode tilt-to-tune
    
    // Client monitoring control
    static void enterClientMonitor();    // Enter overlay mode
    static void exitClientMonitor();     // Return to spectrum
    static void deauthClient(int idx);   // Send deauth burst to selected client
    static void enterRevealMode();       // Start broadcast deauth to discover clients
    static void exitRevealMode();        // Stop reveal mode
    static void updateRevealMode();      // Send periodic broadcast deauths
    
    // Attack mode (PMKID + full handshake capture)
    static void enterAttackMode();       // Start targeted attack on monitored AP
    static void exitAttackMode();        // Stop attack, return to monitor
    static void updateAttackMode();      // Deauth loop + PMKID/handshake check
    static void handleAttackInput();     // Input during attack mode
    static void autoSaveCaptures();      // Save PMKIDs + handshakes to SD
    static void drawAttackOverlay(DisplayCanvas& canvas);  // Attack status screen
    static bool enqueueAttackPMKID(const uint8_t* bssid, const uint8_t* station,
                                   const uint8_t* pmkid, const char* ssid);
    static bool dequeueAttackPMKID(PendingAttackPMKID& out);
    static bool queueHandshakeFrame(const uint8_t* bssid, const uint8_t* station,
                                    const uint8_t* eapolPayload, uint16_t eapolLen,
                                    const uint8_t* fullFrame, uint16_t fullFrameLen,
                                    uint8_t messageNum, int8_t rssi);
    static bool dequeueHandshakeFrame(PendingHandshakeEntry& out);
    static bool saveHandshakePCAP(const SpectrumCapturedHandshake& hs, const char* path);
    static bool saveHandshake22000(const SpectrumCapturedHandshake& hs, const char* path);
    static void writePCAPHeader(File& f);
    static void writePCAPPacket(File& f, const uint8_t* data, uint16_t len, uint32_t ts);
    
    // Data frame processing
    static void processDataFrame(const uint8_t* payload, uint16_t len, int8_t rssi);
    static void trackClient(const uint8_t* bssid, const uint8_t* clientMac, int8_t rssi);
    
    // Coordinate mapping
    static int freqToX(float freqMHz);
    static int rssiToY(int8_t rssi);
    static float channelToFreq(uint8_t channel);
    
    // Security helpers
    static bool isVulnerable(wifi_auth_mode_t mode);
    static const char* authModeToShortString(wifi_auth_mode_t mode);
    static bool detectPMF(const uint8_t* payload, uint16_t len);
    static void detectPMFBits(const uint8_t* payload, uint16_t len, bool& mfpc, bool& mfpr);
    static bool matchesFilter(const SpectrumNetwork& net);  // Check if network passes filter
    static bool matchesFilterRender(const SpectrumRenderNet& net);
    static void updateRenderSnapshot();
    
    // Packet callback for visualization (called by NetworkRecon)
    static void promiscuousCallback(const wifi_promiscuous_pkt_t* pkt, wifi_promiscuous_pkt_type_t type);
};
