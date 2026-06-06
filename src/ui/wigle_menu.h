1|// WiGLE Menu - View wardriving files with sync support
2|#pragma once
3|
4|#include <Arduino.h>
5|6|#include <vector>
7|#include <FS.h>
8|#include <SD.h>
9|
10|// Upload status for display
11|enum class WigleFileStatus {
12|    LOCAL,      // Not uploaded yet
13|    UPLOADED    // Uploaded to WiGLE
14|};
15|
16|struct WigleFileInfo {
17|    char filename[48];
18|    char fullPath[80];
19|    uint32_t fileSize;
20|    uint32_t networkCount;  // Approximate based on file size
21|    WigleFileStatus status;
22|};
23|
24|// Sync state machine for WiGLE operations
25|enum class WigleSyncState {
26|    IDLE,
27|    CONNECTING_WIFI,
28|    FREEING_MEMORY,
29|    UPLOADING,
30|    FETCHING_STATS,
31|    COMPLETE,
32|    ERROR
33|};
34|
35|class WigleMenu {
36|public:
37|    static void init();
38|    static void show();
39|    static void hide();
40|    static void update();
41|    static void draw(M5Canvas& canvas);
42|    static bool isActive() { return active; }
43|    static size_t getCount() { return files.size(); }
44|    static void getSelectedInfo(char* out, size_t len);
45|    
46|private:
47|    static std::vector<WigleFileInfo> files;
48|    static uint8_t selectedIndex;
49|    static uint8_t scrollOffset;
50|    static bool active;
51|    static bool keyWasPressed;
52|    static bool detailViewActive;   // File detail view
53|    static bool nukeConfirmActive;  // Nuke confirmation modal
54|    
55|    static const uint8_t VISIBLE_ITEMS = 5;
56|    
57|    static void scanFiles();
58|    static void handleInput();
59|    static void drawDetailView(M5Canvas& canvas);
60|    static void drawNukeConfirm(M5Canvas& canvas);
61|    static void nukeTrack();
62|    static void formatSize(char* out, size_t len, uint32_t bytes);
63|    
64|    // Async scan state
65|    static bool scanInProgress;
66|    static unsigned long lastScanTime;
67|    static const unsigned long SCAN_DELAY = 50; // ms between scan chunks
68|    static File scanDir;
69|    static File currentFile;
70|    static bool scanComplete;
71|    static size_t scanProgress;
72|    static const size_t SCAN_CHUNK_SIZE = 5; // files to process per chunk
73|    
74|    // Async scan processing
75|    static void processAsyncScan();
76|    
77|    // WiGLE Sync modal state
78|    static bool syncModalActive;
79|    static WigleSyncState syncState;
80|    static char syncStatusText[48];
81|    static uint8_t syncProgress;
82|    static uint8_t syncTotal;
83|    static unsigned long syncStartTime;
84|    static uint8_t syncUploaded;
85|    static uint8_t syncFailed;
86|    static uint8_t syncSkipped;
87|    static bool syncStatsFetched;
88|    static char syncError[48];
89|    
90|    // Sync operations
91|    static void startSync();
92|    static void processSyncState();
93|    static void drawSyncModal(M5Canvas& canvas);
94|    static void cancelSync();
95|    static bool connectToWiFi();
96|    static void disconnectWiFi();
97|    
98|    // Sync progress callback (static for C-style callback)
99|    static void onSyncProgress(const char* status, uint8_t progress, uint8_t total);
100|};
101|