1|// Stress Test Module - Inject fake data to test modes without RF
2|#include "stress_test.h"
3|#include "../modes/spectrum.h"
4|#include "../modes/oink.h"
5|#include "../modes/donoham.h"
6|#include "../ui/display.h"
7|#include "heap_policy.h"
8|// No M5Cardputer on ESP32-S3 Mini
9|
10|// Static member definitions
11|bool StressTest::active = false;
12|StressScenario StressTest::scenario = StressScenario::IDLE;
13|uint32_t StressTest::lastInjectTime = 0;
14|uint32_t StressTest::injectedCount = 0;
15|uint32_t StressTest::injectRate = 0;
16|uint32_t StressTest::lastRateCalc = 0;
17|uint32_t StressTest::injectsSinceLastCalc = 0;
18|uint8_t StressTest::networkCounter = 0;
19|uint8_t StressTest::clientCounter = 0;
20|
21|// Stress guardrails (keep test heavy without crashing the device)
22|static const size_t STRESS_MAX_OINK_NETWORKS = 75;
23|static const size_t STRESS_MAX_DNH_NETWORKS = 60;
24|
25|// Realistic SSID pool
26|const char* StressTest::ssidPool[] = {
27|    "NETGEAR", "linksys", "ATT-WIFI", "xfinitywifi", "ORBI",
28|    "MySpectrumWiFi", "Verizon_5G", "DIRECT-TV", "HP-Print",
29|    "Ring-12ab34", "Nest-Audio", "Chromecast", "Amazon-Fire",
30|    "Tesla-Guest", "Starlink", "5G_Home", "CenturyLink",
31|    "Frontier_WiFi", "Cox_Guest", "Optimum_WiFi", "T-Mobile_Home",
32|    "GoogleFiber", "AT&T_5G", "Hidden_Network", "FBI_VAN",
33|    "PrettyFlyForAWiFi", "GetOffMyLAN", "TheLANBeforeTime",
34|    "WuTangLAN", "BillWiTheScienceFi", "LANDownUnder"
35|};
36|const uint8_t StressTest::ssidPoolSize = sizeof(ssidPool) / sizeof(ssidPool[0]);
37|
38|void StressTest::init() {
39|    active = false;
40|    scenario = StressScenario::IDLE;
41|    lastInjectTime = 0;
42|    injectedCount = 0;
43|    injectRate = 0;
44|    lastRateCalc = millis();
45|    injectsSinceLastCalc = 0;
46|    networkCounter = 0;
47|    clientCounter = 0;
48|}
49|
50|void StressTest::checkActivation() {
51|    // Disabled: stress test activation removed to reduce heap churn risk
52|}
53|
54|void StressTest::setScenario(StressScenario s) {
55|    scenario = s;
56|    Serial.printf("[STRESS] Scenario: %d\n", (int)s);
57|}
58|
59|void StressTest::nextScenario() {
60|    // Disabled
61|}
62|
63|void StressTest::update() {
64|    if (!active || scenario == StressScenario::IDLE) return;
65|    
66|    uint32_t now = millis();
67|    
68|    // Calculate injection rate (per second)
69|    if (now - lastRateCalc >= 1000) {
70|        injectRate = injectsSinceLastCalc;
71|        injectsSinceLastCalc = 0;
72|        lastRateCalc = now;
73|    }
74|    
75|    // Inject interval depends on scenario
76|    uint32_t interval = 50;  // Default: 20/sec
77|    switch (scenario) {
78|        case StressScenario::NETWORK_FLOOD:  interval = 20;  break;  // 50/sec
79|        case StressScenario::CLIENT_FLOOD:   interval = 30;  break;  // 33/sec
80|        case StressScenario::CHURN:          interval = 100; break;  // 10/sec
81|        case StressScenario::HIDDEN_REVEAL:  interval = 200; break;  // 5/sec
82|        case StressScenario::RSSI_CHAOS:     interval = 10;  break;  // 100/sec
83|        case StressScenario::MIXED_AUTH:     interval = 50;  break;  // 20/sec
84|        default: break;
85|    }
86|    
87|    if (now - lastInjectTime < interval) return;
88|    lastInjectTime = now;
89|    
90|    // Execute scenario
91|    switch (scenario) {
92|        case StressScenario::NETWORK_FLOOD:
93|            injectNetwork();
94|            break;
95|        case StressScenario::CLIENT_FLOOD:
96|            injectClient();
97|            break;
98|        case StressScenario::CHURN:
99|            updateChurn();
100|            break;
101|        case StressScenario::HIDDEN_REVEAL:
102|            injectHidden();
103|            break;
104|        case StressScenario::RSSI_CHAOS:
105|            updateRSSIChaos();
106|            break;
107|        case StressScenario::MIXED_AUTH:
108|            injectNetwork();  // Same as flood but with mixed auth
109|            break;
110|        default:
111|            break;
112|    }
113|    
114|    injectedCount++;
115|    injectsSinceLastCalc++;
116|}
117|
118|void StressTest::injectNetwork() {
119|    if (ESP.getFreeHeap() < HeapPolicy::kStressMinHeap) {
120|        return;
121|    }
122|
123|    uint8_t bssid[6];
124|    randomBSSID(bssid);
125|    
126|    wifi_auth_mode_t auth = (scenario == StressScenario::MIXED_AUTH) 
127|        ? randomAuthMode() 
128|        : WIFI_AUTH_WPA2_PSK;
129|    
130|    bool hasPMF = (random(100) < 20);  // 20% have PMF
131|    uint8_t channel = randomChannel();
132|    int8_t rssi = randomRSSI();
133|    const char* ssid = randomSSID();
134|    
135|    // Inject into whichever mode is running
136|    if (SpectrumMode::isRunning()) {
137|        SpectrumMode::onBeacon(bssid, channel, true, rssi, ssid, auth, hasPMF, false);
138|    }
139|    if (OinkMode::isRunning() && OinkMode::getNetworkCount() < STRESS_MAX_OINK_NETWORKS) {
140|        OinkMode::injectTestNetwork(bssid, ssid, channel, rssi, auth, hasPMF);
141|    }
142|    if (DoNoHamMode::isRunning() && DoNoHamMode::getNetworkCount() < STRESS_MAX_DNH_NETWORKS) {
143|        DoNoHamMode::injectTestNetwork(bssid, ssid, channel, rssi, auth, hasPMF);
144|    }
145|}
146|
147|void StressTest::injectClient() {
148|    // Only works if spectrum is monitoring
149|    if (!SpectrumMode::isRunning() || !SpectrumMode::isMonitoring()) {
150|        return;
151|    }
152|    
153|    // We can't directly inject clients - they come from data frames
154|    // Instead, we inject a fake beacon to keep the network alive
155|    // TODO: Add client injection API to SpectrumMode if needed
156|    injectNetwork();
157|}
158|
159|void StressTest::updateChurn() {
160|    // Alternate between adding new networks and letting old ones expire
161|    static uint8_t phase = 0;
162|    phase = (phase + 1) % 10;
163|    
164|    if (phase < 7) {
165|        // 70% of the time: add networks
166|        injectNetwork();
167|    }
168|    // 30%: do nothing, let prune remove stale ones
169|}
170|
171|void StressTest::injectHidden() {
172|    if (ESP.getFreeHeap() < HeapPolicy::kStressMinHeap) {
173|        return;
174|    }
175|
176|    uint8_t bssid[6];
177|    randomBSSID(bssid);
178|    
179|    static uint8_t revealCounter = 0;
180|    revealCounter++;
181|    
182|    uint8_t channel = randomChannel();
183|    int8_t rssi = randomRSSI();
184|    
185|    // First time: hidden (no SSID), every 3rd: reveal
186|    bool reveal = (revealCounter % 3 == 0);
187|    const char* ssid = reveal ? "REVEALED_HIDDEN" : "";
188|    
189|    if (SpectrumMode::isRunning()) {
190|        SpectrumMode::onBeacon(bssid, channel, true, rssi, ssid, WIFI_AUTH_WPA2_PSK, false, reveal);
191|    }
192|    if (OinkMode::isRunning() && OinkMode::getNetworkCount() < STRESS_MAX_OINK_NETWORKS) {
193|        OinkMode::injectTestNetwork(bssid, ssid, channel, rssi, WIFI_AUTH_WPA2_PSK, false);
194|    }
195|    if (DoNoHamMode::isRunning() && DoNoHamMode::getNetworkCount() < STRESS_MAX_DNH_NETWORKS) {
196|        DoNoHamMode::injectTestNetwork(bssid, ssid, channel, rssi, WIFI_AUTH_WPA2_PSK, false);
197|    }
198|}
199|
200|void StressTest::updateRSSIChaos() {
201|    if (ESP.getFreeHeap() < HeapPolicy::kStressMinHeap) {
202|        return;
203|    }
204|
205|    // Re-inject existing networks with wildly varying RSSI
206|    // This tests UI stability with rapid signal changes
207|    uint8_t bssid[6];
208|    // Use low counter to hit same BSSIDs repeatedly
209|    bssid[0] = 0xAA;
210|    bssid[1] = 0xBB;
211|    bssid[2] = 0xCC;
212|    bssid[3] = 0x00;
213|    bssid[4] = 0x00;
214|    bssid[5] = networkCounter % 10;  // Only 10 unique networks
215|    
216|    int8_t rssi = randomRSSI();
217|    
218|    if (SpectrumMode::isRunning()) {
219|        SpectrumMode::onBeacon(bssid, 6, true, rssi, "RSSI_TEST", WIFI_AUTH_WPA2_PSK, false, false);
220|    }
221|    if (OinkMode::isRunning() && OinkMode::getNetworkCount() < STRESS_MAX_OINK_NETWORKS) {
222|        OinkMode::injectTestNetwork(bssid, "RSSI_TEST", 6, rssi, WIFI_AUTH_WPA2_PSK, false);
223|    }
224|    if (DoNoHamMode::isRunning() && DoNoHamMode::getNetworkCount() < STRESS_MAX_DNH_NETWORKS) {
225|        DoNoHamMode::injectTestNetwork(bssid, "RSSI_TEST", 6, rssi, WIFI_AUTH_WPA2_PSK, false);
226|    }
227|}
228|
229|// === Random Data Generators ===
230|
231|void StressTest::randomBSSID(uint8_t* bssid) {
232|    // Use recognizable OUI prefix for stress test networks
233|    bssid[0] = 0xDE;  // "DE:AD:..."
234|    bssid[1] = 0xAD;
235|    bssid[2] = 0xBE;
236|    bssid[3] = (networkCounter >> 8) & 0xFF;
237|    bssid[4] = networkCounter & 0xFF;
238|    bssid[5] = random(256);
239|    networkCounter++;
240|}
241|
242|void StressTest::randomMAC(uint8_t* mac) {
243|    mac[0] = 0xCA;  // "CA:FE:..."
244|    mac[1] = 0xFE;
245|    mac[2] = 0xBA;
246|    mac[3] = 0xBE;
247|    mac[4] = (clientCounter >> 8) & 0xFF;
248|    mac[5] = clientCounter & 0xFF;
249|    clientCounter++;
250|}
251|
252|int8_t StressTest::randomRSSI() {
253|    // Random RSSI between -90 and -30 dBm
254|    return -90 + random(60);
255|}
256|
257|uint8_t StressTest::randomChannel() {
258|    return 1 + random(13);  // Channels 1-13
259|}
260|
261|wifi_auth_mode_t StressTest::randomAuthMode() {
262|    uint8_t r = random(100);
263|    if (r < 10) return WIFI_AUTH_OPEN;
264|    if (r < 15) return WIFI_AUTH_WEP;
265|    if (r < 25) return WIFI_AUTH_WPA_PSK;
266|    if (r < 60) return WIFI_AUTH_WPA2_PSK;
267|    if (r < 80) return WIFI_AUTH_WPA_WPA2_PSK;
268|    return WIFI_AUTH_WPA3_PSK;
269|}
270|
271|const char* StressTest::randomSSID() {
272|    return ssidPool[random(ssidPoolSize)];
273|}
274|