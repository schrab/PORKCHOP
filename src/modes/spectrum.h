1|// HOG ON SPECTRUM Mode - WiFi Spectrum Analyzer
2|#pragma once
3|
4|#include <Arduino.h>
5|6|#include <vector>
7|#include <atomic>
8|#include <esp_wifi.h>
9|#include <esp_wifi_types.h>
10|
11|// Client monitoring constants
12|#define MAX_SPECTRUM_CLIENTS 8
13|#define MAX_SPECTRUM_NETWORKS 64  // Reduced from 100 for cleaner display
14|#define CLIENT_STALE_TIMEOUT_MS 30000  // 30s before client considered gone
15|#define VISIBLE_CLIENTS 4              // How many fit on screen
16|#define SIGNAL_LOST_TIMEOUT_MS 15000   // 15s no beacon = signal lost
17|#define CLIENT_BEEP_LIMIT 4            // Only beep for first N clients
18|
19|// Client tracking for monitored network
20|struct SpectrumClient {
21|    uint8_t mac[6];
22|    int8_t rssi;
23|    uint32_t lastSeen;
24|    const char* vendor;  // Cached OUI lookup
25|};
26|
27|struct SpectrumNetwork {
28|    uint8_t bssid[6];
29|    char ssid[33];
30|    uint8_t channel;         // 1-13
31|    int8_t rssi;             // Latest RSSI
32|    uint32_t lastSeen;       // millis() of last beacon
33|    wifi_auth_mode_t authmode; // Security type (OPEN/WEP/WPA/WPA2/WPA3)
34|    bool hasPMF;             // Protected Management Frames (immune to deauth)
35|    bool isHidden;           // Hidden SSID (beacon had empty SSID)
36|    bool wasRevealed;        // SSID was revealed via probe response
37|    float displayFreqMHz;    // Smoothed frequency for rendering (prevents left/right jitter)
38|    // Client tracking (only populated when monitoring THIS network)
39|    SpectrumClient clients[MAX_SPECTRUM_CLIENTS];
40|    uint8_t clientCount;
41|};
42|
43|// Render snapshot (heap-safe, no vector pointers)
44|struct SpectrumRenderNet {
45|    uint8_t bssid[6];
46|    uint8_t channel;
47|    int8_t rssi;
48|    wifi_auth_mode_t authmode;
49|    bool hasPMF;
50|    bool isHidden;
51|    float displayFreqMHz;
52|};
53|
54|struct SpectrumRenderSelected {
55|    bool valid;
56|    uint8_t bssid[6];
57|    char ssid[33];
58|    uint8_t channel;
59|    int8_t rssi;
60|    wifi_auth_mode_t authmode;
61|    bool hasPMF;
62|    bool wasRevealed;
63|};
64|
65|struct SpectrumRenderMonitor {
66|    bool valid;
67|    uint8_t bssid[6];
68|    char ssid[33];
69|    uint8_t channel;
70|    int8_t rssi;
71|    uint8_t clientCount;
72|    SpectrumClient clients[MAX_SPECTRUM_CLIENTS];
73|};
74|
75|// MAC comparison helper [P8]
76|inline bool macEqual(const uint8_t* a, const uint8_t* b) {
77|    return memcmp(a, b, 6) == 0;
78|}
79|
80|// Filter modes for target selection
81|enum class SpectrumFilter : uint8_t {
82|    ALL = 0,   // Show all networks
83|    VULN,      // OPEN/WEP/WPA only (weak security)
84|    SOFT,      // No PMF (deauth-able)
85|    HIDDEN     // Hidden SSIDs only
86|};
87|
88|class SpectrumMode {
89|public:
90|    static void init();
91|    static void start();
92|    static void stop();
93|    static void update();
94|    static void draw(DisplayCanvas& canvas);
95|    static bool isRunning() { return running; }
96|    
97|    // For promiscuous callback - updates network RSSI
98|    static void onBeacon(const uint8_t* bssid, uint8_t channel, bool channelTrusted, int8_t rssi, const char* ssid, wifi_auth_mode_t authmode, bool hasPMF, bool isProbeResponse);
99|    
100|    // Bottom bar info
101|    static void getSelectedInfo(char* out, size_t len);
102|    
103|    // Client monitoring accessors [P3]
104|    static bool isMonitoring() { return monitoringNetwork; }
105|    static const char* getMonitoredSSID();
106|    static int getClientCount();
107|    static uint8_t getMonitoredChannel() { return monitoredChannel; }
108|    
109|private:
110|    static bool running;
111|    static std::atomic<bool> busy;   // Guard against callback race (atomic for cross-core visibility)
112|    static std::vector<SpectrumNetwork> networks;
113|    static SpectrumRenderNet renderNets[MAX_SPECTRUM_NETWORKS];
114|    static uint16_t renderCount;
115|    static SpectrumRenderSelected renderSelected;
116|    static SpectrumRenderMonitor renderMonitor;
117|    static float viewCenterMHz;      // Center of visible spectrum
118|    static float viewWidthMHz;       // Visible bandwidth
119|    static int selectedIndex;        // Currently highlighted network
120|    static uint32_t lastUpdateTime;
121|    static bool keyWasPressed;
122|    static uint8_t currentChannel;   // Current hop channel
123|    static uint32_t startTime;       // When mode started (for achievement)
124|    
125|    // Filter state
126|    static SpectrumFilter filter;    // Current filter mode
127|    
128|    // Deferred logging for revealed SSIDs (avoid Serial in callback)
129|    static volatile bool pendingReveal;
130|    static char pendingRevealSSID[33];
131|    
132|    // Deferred network add (avoid push_back in callback - ESP32 dual-core race)
133|    static std::atomic<bool> pendingNetworkAdd;  // Atomic for cross-core visibility (WiFi task → main loop)
134|    static SpectrumNetwork pendingNetwork;
135|    
136|    // Client monitoring state [P1] [P2]
137|    static bool monitoringNetwork;       // True when locked on network
138|    static int monitoredNetworkIndex;    // Index of network being monitored
139|    static uint8_t monitoredBSSID[6];    // [P2] Store BSSID, not just index!
140|    static uint8_t monitoredChannel;     // Locked channel
141|    static int clientScrollOffset;       // For scrolling client list
142|    static int selectedClientIndex;      // Currently highlighted client
143|    static uint32_t lastClientPrune;     // Last stale client cleanup
144|    static uint8_t clientsDiscoveredThisSession;  // For limiting beeps
145|    static volatile bool pendingClientBeep;       // Deferred beep for new client
146|    static volatile uint8_t pendingNetworkXP;     // Deferred XP for new networks (avoids callback crash)
147|    
148|    // Achievement tracking for client monitor (v0.1.6)
149|    static uint32_t clientMonitorEntryTime;  // When we entered client monitor
150|    static uint8_t deauthsThisMonitor;       // Deauths since entering monitor
151|    static uint32_t firstDeauthTime;         // Time of first deauth (for QUICK_DRAW)
152|    
153|    // Client detail popup state
154|    static bool clientDetailActive;          // Detail popup visible
155|    static uint8_t detailClientMAC[6];       // MAC of client being viewed (close if changes)
156|    
157|    // Reveal mode state (broadcast deauth to discover clients)
158|    static bool revealingClients;            // True when in reveal mode
159|    static uint32_t revealStartTime;         // When reveal mode started
160|    static uint32_t lastRevealBurst;         // Last broadcast deauth time
161|    
162|    // Dial mode state (tilt-to-tune when device upright)
163|    static bool dialMode;                    // Auto-enabled when UPS (upright)
164|    static bool dialLocked;                  // Channel lock (space toggles)
165|    static bool dialWasUpright;              // Hysteresis state for FLT/UPS detection
166|    static uint8_t dialChannel;              // Current dial channel (1-13)
167|    static float dialPositionTarget;         // Raw gyro position (1.0-13.0)
168|    static float dialPositionSmooth;         // Lerped display position (smooth)
169|    static uint32_t lastDialUpdate;          // Timing for lerp
170|    static uint32_t dialModeEntryTime;       // When dial mode was entered (debounce)
171|    static volatile uint32_t ppsCounter;     // Packet counter (callback increments)
172|    static uint32_t displayPps;              // Displayed pps (updated per second)
173|    static uint32_t lastPpsUpdate;           // Last pps calculation time
174|    
175|    static void handleInput();
176|    static void handleClientMonitorInput();  // Input when monitoring
177|    static void drawSpectrum(DisplayCanvas& canvas);
178|    static void drawClientOverlay(DisplayCanvas& canvas);  // Client list overlay
179|    static void drawClientDetail(DisplayCanvas& canvas);   // Client detail popup
180|    static void drawGaussianLobe(DisplayCanvas& canvas, float centerFreqMHz, int8_t rssi, bool filled, uint16_t activityPps, uint8_t seed);
181|    static void drawAxis(DisplayCanvas& canvas);
182|    static void drawChannelMarkers(DisplayCanvas& canvas);
183|    static void drawFilterBar(DisplayCanvas& canvas);     // Filter indicator bar
184|    static void drawDialInfo(DisplayCanvas& canvas);      // Dial mode info bar
185|    static void drawNoiseFloor(DisplayCanvas& canvas);    // Animated noise at baseline
186|    static void drawWaterfall(DisplayCanvas& canvas);     // Historical spectrum waterfall
187|    static void updateSpectrumBuffers();             // Populate buffers from network data
188|    static void updateWaterfall();                   // Push to waterfall history
189|    static void pruneStale();            // Remove networks not seen recently
190|    static void pruneStaleClients();     // Remove clients not seen recently
191|    static void updateDialChannel();     // Update dial mode tilt-to-tune
192|    
193|    // Client monitoring control
194|    static void enterClientMonitor();    // Enter overlay mode
195|    static void exitClientMonitor();     // Return to spectrum
196|    static void deauthClient(int idx);   // Send deauth burst to selected client
197|    static void enterRevealMode();       // Start broadcast deauth to discover clients
198|    static void exitRevealMode();        // Stop reveal mode
199|    static void updateRevealMode();      // Send periodic broadcast deauths
200|    
201|    // Data frame processing
202|    static void processDataFrame(const uint8_t* payload, uint16_t len, int8_t rssi);
203|    static void trackClient(const uint8_t* bssid, const uint8_t* clientMac, int8_t rssi);
204|    
205|    // Coordinate mapping
206|    static int freqToX(float freqMHz);
207|    static int rssiToY(int8_t rssi);
208|    static float channelToFreq(uint8_t channel);
209|    
210|    // Security helpers
211|    static bool isVulnerable(wifi_auth_mode_t mode);
212|    static const char* authModeToShortString(wifi_auth_mode_t mode);
213|    static bool detectPMF(const uint8_t* payload, uint16_t len);
214|    static void detectPMFBits(const uint8_t* payload, uint16_t len, bool& mfpc, bool& mfpr);
215|    static bool matchesFilter(const SpectrumNetwork& net);  // Check if network passes filter
216|    static bool matchesFilterRender(const SpectrumRenderNet& net);
217|    static void updateRenderSnapshot();
218|    
219|    // Packet callback for visualization (called by NetworkRecon)
220|    static void promiscuousCallback(const wifi_promiscuous_pkt_t* pkt, wifi_promiscuous_pkt_type_t type);
221|};
222|