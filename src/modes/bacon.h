1|// Bacon Mode - Hide and Seek Beacon Broadcaster
2|// Broadcasts WiFi beacons on CH:6 with AP fingerprint (Vendor IE)
3|#pragma once
4|
5|#include <Arduino.h>
6|#include <esp_wifi.h>
7|8|
9|// Constants
10|#define BACON_CHANNEL 6
11|#define BACON_JITTER_MAX 50        // ms random jitter (0-50)
12|#define BACON_MAX_APS 3            // Top 3 APs for fingerprint
13|
14|// Beacon TX tiers (key 1/2/3)
15|#define BACON_TIER1_MS 50          // Fast
16|#define BACON_TIER2_MS 100         // Balanced
17|#define BACON_TIER3_MS 150         // Slow
18|
19|// AP fingerprint structure (what we broadcast)
20|struct BaconAPInfo {
21|    uint8_t bssid[6];
22|    int8_t rssi;        // RSSI as seen by Porkchop
23|    uint8_t channel;
24|    char ssid[33];      // AP name (for display)
25|};
26|
27|// Vendor IE structure for beacon (OUI: 0x50:52:4B = "PRK")
28|struct BaconVendorIE {
29|    uint8_t elementId;      // 0xDD (Vendor Specific)
30|    uint8_t length;         // 5 + (apCount * 8)
31|    uint8_t oui[3];         // {0x50, 0x52, 0x4B}
32|    uint8_t type;           // 0x01 (Bacon mode)
33|    uint8_t apCount;        // 1-3
34|    BaconAPInfo aps[BACON_MAX_APS];
35|} __attribute__((packed));
36|
37|class BaconMode {
38|public:
39|    static void init();
40|    static void start();
41|    static void stop();
42|    static void update();
43|    static void draw(M5Canvas& canvas);  // Called by Display::update()
44|    static bool isRunning() { return running; }
45|    
46|    // Getters for Display bottom bar
47|    static uint32_t getBeaconCount() { return beaconCount; }
48|    static uint32_t getSessionTime() { return running ? (millis() - sessionStartTime) / 1000 : 0; }
49|    static float getBeaconRate();  // Beacons per second
50|    static uint8_t getAPCount() { return apCount; }
51|    static const BaconAPInfo* getAPList() { return apFingerprint; }
52|    static uint8_t getCurrentTier() { return currentTier; }
53|    static uint16_t getCurrentInterval() { return beaconInterval; }
54|    
55|private:
56|    static bool running;
57|    static uint32_t beaconCount;
58|    static uint32_t lastBeaconTime;
59|    static uint32_t sessionStartTime;
60|    static uint16_t sequenceNumber;
61|    static BaconAPInfo apFingerprint[BACON_MAX_APS];
62|    static uint8_t apCount;
63|    static uint8_t currentTier;      // 1-3
64|    static uint16_t beaconInterval;  // Current interval in ms
65|    static uint32_t lastStatusMessageTime;
66|    static uint8_t statusCycleIndex;
67|    static int8_t lastGeneralPhraseIdx;
68|    static bool scanInProgress;
69|    static bool scanCompleted;
70|    static uint32_t scanStartTime;
71|    static bool reconWasRunning;
72|    static bool reconWasPaused;
73|    
74|    // Internal methods
75|    static void startAsyncScan();
76|    static void updateAsyncScan();
77|    static void buildBeaconFrame(uint8_t* buffer, size_t* len);
78|    static void buildVendorIE(uint8_t* buffer, size_t* len, uint8_t apCountOverride);
79|    static void sendBeacon();
80|    static void handleInput();
81|    static void updateStatusMessage();
82|};
83|
84|
85|