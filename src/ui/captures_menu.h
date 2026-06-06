1|// Captures Menu - View saved handshake captures
2|#pragma once
3|
4|#include <Arduino.h>
5|6|#include <vector>
7|#include <FS.h>
8|#include <SD.h>
9|
10|// WPA-SEC status for display
11|enum class CaptureStatus {
12|    LOCAL,      // Not uploaded yet
13|    UPLOADED,   // Uploaded, waiting for crack
14|    CRACKED     // Password found!
15|};
16|
17|struct CaptureInfo {
18|    char filename[48];
19|    char ssid[33];
20|    char bssid[18];
21|    uint32_t fileSize;
22|    time_t captureTime;  // File modification time
23|    bool isPMKID;        // true = .22000 PMKID, false = .pcap handshake
24|    CaptureStatus status; // WPA-SEC status
25|    char password[64];    // Cracked password (if status == CRACKED)
26|};
27|
28|// Sync state machine for WPA-SEC operations
29|enum class SyncState {
30|    IDLE,
31|    CONNECTING_WIFI,
32|    FREEING_MEMORY,
33|    UPLOADING,
34|    DOWNLOADING_POTFILE,
35|    COMPLETE,
36|    ERROR
37|};
38|
39|class CapturesMenu {
40|public:
41|    static void init();
42|    static void show();
43|    static void hide();
44|    static void update();
45|    static void draw(M5Canvas& canvas);
46|    
47|    // Emergency cleanup for low heap situations
48|    static void emergencyCleanup();
49|    static bool isActive() { return active; }
50|    static const char* getSelectedBSSID();
51|    static size_t getCount() { return captures.size(); }
52|    
53|private:
54|    static std::vector<CaptureInfo> captures;
55|    static uint8_t selectedIndex;
56|    static uint8_t scrollOffset;
57|    static bool active;
58|    static bool keyWasPressed;
59|    static bool nukeConfirmActive;  // Nuke confirmation modal
60|    static bool detailViewActive;   // Password detail view
61|    
62|    static const uint8_t VISIBLE_ITEMS = 5;
63|    
64|    static bool scanCaptures();  // Returns true if successful, false if SD access failed
65|    static void handleInput();
66|    static void drawNukeConfirm(M5Canvas& canvas);
67|    static void drawDetailView(M5Canvas& canvas);
68|    static void nukeLoot();
69|    static void updateWPASecStatus();
70|    static void formatTime(char* out, size_t len, time_t t);
71|    static const size_t MAX_CAPTURES = 100;
72|    
73|    // Async scan state
74|    static bool scanInProgress;
75|    static unsigned long lastScanTime;
76|    static const unsigned long SCAN_DELAY = 50; // ms between scan chunks
77|    static File scanDir;
78|    static File currentFile;
79|    static bool scanComplete;
80|    static size_t scanProgress;
81|    static const size_t SCAN_CHUNK_SIZE = 5; // files to process per chunk
82|    
83|    // Async scan processing
84|    static void processAsyncScan();
85|    
86|    // Async WPA-SEC status update state
87|    static bool wpasecUpdateInProgress;
88|    static unsigned long lastWpasecUpdateTime;
89|    static size_t wpasecUpdateProgress;
90|    static const unsigned long WPASEC_UPDATE_DELAY = 25; // ms between updates
91|    static const size_t WPASEC_UPDATE_CHUNK_SIZE = 3; // captures to process per chunk
92|    
93|    // Async WPA-SEC status update processing
94|    static void processAsyncWPASecUpdate();
95|    
96|    // WPA-SEC Sync modal state
97|    static bool syncModalActive;
98|    static SyncState syncState;
99|    static char syncStatusText[48];
100|    static uint8_t syncProgress;
101|    static uint8_t syncTotal;
102|    static unsigned long syncStartTime;
103|    static uint8_t syncUploaded;
104|    static uint8_t syncFailed;
105|    static uint16_t syncCracked;
106|    static char syncError[48];
107|    
108|    // Sync operations
109|    static void startSync();
110|    static void processSyncState();
111|    static void drawSyncModal(M5Canvas& canvas);
112|    static void cancelSync();
113|    static bool connectToWiFi();
114|    static void disconnectWiFi();
115|    
116|    // Sync progress callback (static for C-style callback)
117|    static void onSyncProgress(const char* status, uint8_t progress, uint8_t total);
118|
119|    // Hint rotation
120|    static uint8_t hintIndex;
121|    static const char* const HINTS[];
122|    static const uint8_t HINT_COUNT = 5;
123|};
124|