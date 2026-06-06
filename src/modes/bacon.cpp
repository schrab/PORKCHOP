1|// Bacon Mode - Implementation
2|
3|#include "bacon.h"
4|#include "../hal/hal_input.h"
5|#include <WiFi.h>
6|#include <esp_wifi.h>
7|#include "../ui/display.h"
8|#include "../piglet/mood.h"
9|#include "../piglet/avatar.h"
10|#include "../piglet/weather.h"
11|#include "../core/sdlog.h"
12|#include "../core/xp.h"
13|#include "../core/wifi_utils.h"
14|#include "../core/network_recon.h"
15|
16|// Static member initialization
17|bool BaconMode::running = false;
18|uint32_t BaconMode::beaconCount = 0;
19|uint32_t BaconMode::lastBeaconTime = 0;
20|uint32_t BaconMode::sessionStartTime = 0;
21|uint16_t BaconMode::sequenceNumber = 0;
22|BaconAPInfo BaconMode::apFingerprint[BACON_MAX_APS];
23|uint8_t BaconMode::apCount = 0;
24|uint8_t BaconMode::currentTier = 1;           // Default tier 1 (fast)
25|uint16_t BaconMode::beaconInterval = BACON_TIER1_MS;
26|uint32_t BaconMode::lastStatusMessageTime = 0;
27|uint8_t BaconMode::statusCycleIndex = 0;
28|int8_t BaconMode::lastGeneralPhraseIdx = -1;
29|bool BaconMode::scanInProgress = false;
30|bool BaconMode::scanCompleted = false;
31|uint32_t BaconMode::scanStartTime = 0;
32|bool BaconMode::reconWasRunning = false;
33|bool BaconMode::reconWasPaused = false;
34|
35|static const uint32_t BACON_STATUS_INTERVAL_MS = 5000;
36|static const uint32_t BACON_SCAN_TIMEOUT_MS = 8000;
37|static const uint8_t BACON_STATUS_CYCLE[] = {0, 0, 1, 0, 2};
38|
39|static const char* BACON_PHRASES_GENERAL[] = {
40|    "FATHER ONLINE. HOLD STEADY.",
41|    "WEYLAND NODE. SIGNAL CLEAN.",
42|    "PARENT SIGNAL. KEEP WATCH.",
43|    "LONG GONE POPS. STILL HERE.",
44|    "COLD CORE. WARM CARRIER.",
45|    "AUTOMATON CALM. KEEP TX.",
46|    "KOSHER OK. NO FLESH.",
47|    "HALAL OK. JUST SIGNAL.",
48|    "NO WORRY. BYTE PIG."
49|};
50|
51|static const char* BACON_PHRASES_KEYS[] = {
52|    "KEYS 1 2 3. TIER SHIFT.",
53|    "1 2 3 SET TIER.",
54|    "TIER KEYS 1 2 3."
55|};
56|
57|void BaconMode::init() {
58|    Serial.println("[BACON] Initializing...");
59|    
60|    // Reset state
61|    running = false;
62|    beaconCount = 0;
63|    lastBeaconTime = 0;
64|    sequenceNumber = 0;
65|    apCount = 0;
66|    memset(apFingerprint, 0, sizeof(apFingerprint));
67|    scanInProgress = false;
68|    scanCompleted = false;
69|    scanStartTime = 0;
70|    reconWasRunning = false;
71|    reconWasPaused = false;
72|    
73|    Serial.println("[BACON] Initialized");
74|}
75|
76|void BaconMode::start() {
77|    Serial.println("[BACON] Starting...");
78|    
79|    // Pause NetworkRecon to avoid promiscuous conflicts during scan/tx
80|    // Use pause() instead of stop() to preserve state for lighter resume
81|    reconWasRunning = NetworkRecon::isRunning();
82|    reconWasPaused = NetworkRecon::isPaused();
83|    if (reconWasRunning) {
84|        NetworkRecon::pause();
85|    }
86|
87|    // Show scanning toast and start async scan (non-blocking)
88|    Display::notify(NoticeKind::STATUS, "SCANNING REFS...", 5000, NoticeChannel::TOP_BAR);
89|    startAsyncScan();
90|    
91|    // Setup WiFi for beacon transmission
92|    WiFi.mode(WIFI_MODE_STA);
93|    esp_wifi_set_channel(BACON_CHANNEL, WIFI_SECOND_CHAN_NONE);
94|    delay(100);
95|    
96|    // Show ready toast
97|    Display::notify(NoticeKind::STATUS, "BACON HOT ON CH:6", 5000, NoticeChannel::TOP_BAR);
98|    
99|    // Set running state
100|    running = true;
101|    beaconCount = 0;
102|    sessionStartTime = millis();
103|    lastBeaconTime = millis();
104|    
105|    // Set avatar state
106|    Avatar::setState(AvatarState::HAPPY);
107|    
108|    // Lock auto mood phrases and start FATHER terminal status rotation
109|    Mood::setDialogueLock(true);
110|    lastStatusMessageTime = millis() - BACON_STATUS_INTERVAL_MS;
111|    statusCycleIndex = 2;
112|    lastGeneralPhraseIdx = -1;
113|    updateStatusMessage();
114|    
115|    SDLog::log("BACON", "Started - Broadcasting on CH:6 with %d APs", apCount);
116|}
117|
118|void BaconMode::stop() {
119|    if (!running) return;
120|    
121|    Serial.println("[BACON] Stopping...");
122|    
123|    running = false;
124|
125|    if (scanInProgress) {
126|        WiFi.scanDelete();
127|        scanInProgress = false;
128|        scanCompleted = true;
129|    }
130|    
131|    // Full WiFi shutdown for clean BLE handoff (per BEST_PRACTICES section 14)
132|    // Must stop() recon first since shutdown() kills WiFi out from under it
133|    NetworkRecon::stop();
134|    WiFiUtils::shutdown();
135|
136|    // Restore NetworkRecon state if it was active before BACON
137|    if (reconWasRunning) {
138|        NetworkRecon::start();
139|    } else if (reconWasPaused) {
140|        NetworkRecon::start();
141|        NetworkRecon::pause();
142|    }
143|    reconWasRunning = false;
144|    reconWasPaused = false;
145|    
146|    // Clear bottom bar overlay
147|    Display::clearBottomOverlay();
148|    
149|    // Reset avatar
150|    Avatar::setState(AvatarState::NEUTRAL);
151|    
152|    // Clear mood message
153|    Mood::setStatusMessage("");
154|    Mood::setDialogueLock(false);
155|    
156|    Serial.printf("[BACON] Stopped - Sent %lu beacons\n", beaconCount);
157|    SDLog::log("BACON", "Stopped - Total beacons: %lu", beaconCount);
158|}
159|
160|void BaconMode::update() {
161|    if (!running) return;
162|    
163|    // Handle tier switching input
164|    handleInput();
165|
166|    // Async scan completion
167|    updateAsyncScan();
168|
169|    // Rotate status messages for FATHER terminal
170|    updateStatusMessage();
171|    
172|    // Check if it's time to send next beacon
173|    uint32_t now = millis();
174|    uint32_t interval = beaconInterval + random(0, BACON_JITTER_MAX + 1);
175|    
176|    if (now - lastBeaconTime >= interval) {
177|        sendBeacon();
178|        beaconCount++;
179|        lastBeaconTime = now;
180|    }
181|    
182|    // Note: Draw is handled by Display::update() which calls draw(canvas)
183|}
184|
185|void BaconMode::handleInput() {
186|    hal_input_update();
187|    
188|    if (hal_input_isChange() && hal_input_anyHeld()) {
189|        // replaced: keysState handled via hal_input_getch/wasPressed
190|        
191|        for (auto key : state.word) {
192|            uint8_t newTier = 0;
193|            uint16_t newInterval = 0;
194|            
195|            switch (key) {
196|                case '1':
197|                    newTier = 1;
198|                    newInterval = BACON_TIER1_MS;
199|                    break;
200|                case '2':
201|                    newTier = 2;
202|                    newInterval = BACON_TIER2_MS;
203|                    break;
204|                case '3':
205|                    newTier = 3;
206|                    newInterval = BACON_TIER3_MS;
207|                    break;
208|            }
209|            
210|            if (newTier > 0 && newTier != currentTier) {
211|                currentTier = newTier;
212|                beaconInterval = newInterval;
213|                
214|                char toast[32];
215|                snprintf(toast, sizeof(toast), "TX TIER %d: %dms", currentTier, beaconInterval);
216|                Display::notify(NoticeKind::STATUS, toast, 0, NoticeChannel::TOP_BAR);
217|                
218|                SDLog::log("BACON", "Switched to tier %d (%dms)", currentTier, beaconInterval);
219|            }
220|        }
221|    }
222|}
223|
224|void BaconMode::updateStatusMessage() {
225|    uint32_t now = millis();
226|    if (now - lastStatusMessageTime < BACON_STATUS_INTERVAL_MS) return;
227|    lastStatusMessageTime = now;
228|
229|    uint8_t cycleIdx = statusCycleIndex % (sizeof(BACON_STATUS_CYCLE) / sizeof(BACON_STATUS_CYCLE[0]));
230|    uint8_t mode = BACON_STATUS_CYCLE[cycleIdx];
231|    statusCycleIndex++;
232|
233|    char buf[48];
234|
235|    if (mode == 1) {
236|        // Channel reminder (includes current tier/interval)
237|        snprintf(buf, sizeof(buf), "CH%d TX. T%d %dMS", BACON_CHANNEL, currentTier, beaconInterval);
238|        Mood::setStatusMessage(buf);
239|        return;
240|    }
241|
242|    if (mode == 2) {
243|        int idx = random(0, (int)(sizeof(BACON_PHRASES_KEYS) / sizeof(BACON_PHRASES_KEYS[0])));
244|        Mood::setStatusMessage(BACON_PHRASES_KEYS[idx]);
245|        return;
246|    }
247|
248|    // General FATHER phrases
249|    int count = sizeof(BACON_PHRASES_GENERAL) / sizeof(BACON_PHRASES_GENERAL[0]);
250|    int idx = random(0, count);
251|    if (count > 1 && idx == lastGeneralPhraseIdx) {
252|        idx = (idx + 1) % count;
253|    }
254|    lastGeneralPhraseIdx = idx;
255|    Mood::setStatusMessage(BACON_PHRASES_GENERAL[idx]);
256|}
257|
258|void BaconMode::startAsyncScan() {
259|    if (scanInProgress) return;
260|    apCount = 0;
261|    memset(apFingerprint, 0, sizeof(apFingerprint));
262|    scanCompleted = false;
263|    scanStartTime = millis();
264|    scanInProgress = true;
265|    // Start async scan (results collected later)
266|    WiFi.scanNetworks(true, true);
267|}
268|
269|void BaconMode::updateAsyncScan() {
270|    if (!scanInProgress) return;
271|    if (millis() - scanStartTime > BACON_SCAN_TIMEOUT_MS) {
272|        Serial.println("[BACON] Scan timeout");
273|        WiFi.scanDelete();
274|        scanInProgress = false;
275|        scanCompleted = true;
276|        return;
277|    }
278|    int n = WiFi.scanComplete();
279|    if (n == WIFI_SCAN_RUNNING) {
280|        return;
281|    }
282|    if (n == WIFI_SCAN_FAILED) {
283|        Serial.println("[BACON] Scan failed");
284|        WiFi.scanDelete();
285|        scanInProgress = false;
286|        scanCompleted = true;
287|        return;
288|    }
289|    if (n <= 0) {
290|        Serial.println("[BACON] No APs found");
291|        WiFi.scanDelete();
292|        scanInProgress = false;
293|        scanCompleted = true;
294|        return;
295|    }
296|
297|    Serial.printf("[BACON] Found %d APs\n", n);
298|
299|    // Extract top 3 APs by RSSI
300|    for (int i = 0; i < n && apCount < BACON_MAX_APS; i++) {
301|        int8_t maxRSSI = -128;
302|        int maxIdx = -1;
303|
304|        for (int j = 0; j < n; j++) {
305|            int8_t rssi = WiFi.RSSI(j);
306|
307|            bool alreadyAdded = false;
308|            uint8_t* bssid = WiFi.BSSID(j);
309|            for (int k = 0; k < apCount; k++) {
310|                if (memcmp(apFingerprint[k].bssid, bssid, 6) == 0) {
311|                    alreadyAdded = true;
312|                    break;
313|                }
314|            }
315|
316|            if (!alreadyAdded && rssi > maxRSSI) {
317|                maxRSSI = rssi;
318|                maxIdx = j;
319|            }
320|        }
321|
322|        if (maxIdx >= 0) {
323|            uint8_t* bssid = WiFi.BSSID(maxIdx);
324|            memcpy(apFingerprint[apCount].bssid, bssid, 6);
325|            apFingerprint[apCount].rssi = WiFi.RSSI(maxIdx);
326|            apFingerprint[apCount].channel = WiFi.channel(maxIdx);
327|
328|            String ssid = WiFi.SSID(maxIdx);
329|            strncpy(apFingerprint[apCount].ssid, ssid.c_str(), 32);
330|            apFingerprint[apCount].ssid[32] = 0;
331|
332|            Serial.printf("[BACON] AP %d: %s  %ddB  CH:%d  %02X:%02X:%02X:%02X:%02X:%02X\n",
333|                         apCount + 1,
334|                         ssid.c_str(),
335|                         apFingerprint[apCount].rssi,
336|                         apFingerprint[apCount].channel,
337|                         bssid[0], bssid[1], bssid[2],
338|                         bssid[3], bssid[4], bssid[5]);
339|
340|            apCount++;
341|        }
342|        if ((i & 0x01) == 0) {
343|            delay(1);
344|        }
345|    }
346|
347|    WiFi.scanDelete();
348|    scanInProgress = false;
349|    scanCompleted = true;
350|    Serial.printf("[BACON] Selected %d APs for fingerprint\n", apCount);
351|}
352|
353|void BaconMode::buildVendorIE(uint8_t* buffer, size_t* len, uint8_t apCountOverride) {
354|    // Build Vendor IE structure
355|    size_t offset = 0;
356|    uint8_t count = apCountOverride;
357|    if (count > BACON_MAX_APS) {
358|        count = BACON_MAX_APS;
359|    }
360|    
361|    buffer[offset++] = 0xDD;  // Element ID: Vendor Specific
362|    buffer[offset++] = 0;     // Length (filled later)
363|    
364|    // OUI: 0x50:52:4B (PRK = Porkchop)
365|    buffer[offset++] = 0x50;
366|    buffer[offset++] = 0x52;
367|    buffer[offset++] = 0x4B;
368|    
369|    // Type: 0x01 (Bacon mode)
370|    buffer[offset++] = 0x01;
371|    
372|    // AP count
373|    buffer[offset++] = count;
374|    
375|    // AP data
376|    for (int i = 0; i < count; i++) {
377|        memcpy(&buffer[offset], apFingerprint[i].bssid, 6);
378|        offset += 6;
379|        buffer[offset++] = (uint8_t)apFingerprint[i].rssi;
380|        buffer[offset++] = apFingerprint[i].channel;
381|    }
382|    
383|    // Fill length field (total - 2 for element ID and length field itself)
384|    buffer[1] = offset - 2;
385|    
386|    *len = offset;
387|}
388|
389|void BaconMode::buildBeaconFrame(uint8_t* buffer, size_t* len) {
390|    size_t offset = 0;
391|    const size_t maxLen = 256;
392|    
393|    // Get our MAC address
394|    uint8_t ourMAC[6];
395|    esp_wifi_get_mac(WIFI_IF_STA, ourMAC);
396|    
397|    // === 802.11 MAC Header (24 bytes) ===
398|    
399|    // Frame Control (2 bytes): Type=Management(0), Subtype=Beacon(8)
400|    buffer[offset++] = 0x80;  // Beacon frame
401|    buffer[offset++] = 0x00;
402|    
403|    // Duration (2 bytes)
404|    buffer[offset++] = 0x00;
405|    buffer[offset++] = 0x00;
406|    
407|    // Address 1: Destination (broadcast)
408|    memset(&buffer[offset], 0xFF, 6);
409|    offset += 6;
410|    
411|    // Address 2: Source (our MAC)
412|    memcpy(&buffer[offset], ourMAC, 6);
413|    offset += 6;
414|    
415|    // Address 3: BSSID (our MAC)
416|    memcpy(&buffer[offset], ourMAC, 6);
417|    offset += 6;
418|    
419|    // Sequence Control (2 bytes)
420|    uint16_t seqCtrl = (sequenceNumber << 4);
421|    buffer[offset++] = seqCtrl & 0xFF;
422|    buffer[offset++] = (seqCtrl >> 8) & 0xFF;
423|    sequenceNumber = (sequenceNumber + 1) & 0xFFF;  // 12-bit wrap
424|    
425|    // === Beacon Frame Body ===
426|    
427|    // Timestamp (8 bytes) - will be filled by hardware
428|    memset(&buffer[offset], 0, 8);
429|    offset += 8;
430|    
431|    // Beacon Interval (2 bytes) - 100 TU (102.4ms)
432|    buffer[offset++] = 0x64;
433|    buffer[offset++] = 0x00;
434|    
435|    // Capability Info (2 bytes): ESS + Short Preamble
436|    buffer[offset++] = 0x01;  // ESS
437|    buffer[offset++] = 0x04;  // Short Preamble
438|    
439|    // === Information Elements ===
440|    
441|    // SSID (Tag 0)
442|    buffer[offset++] = 0x00;  // Tag: SSID
443|    buffer[offset++] = 0x10;  // Length: 16
444|    memcpy(&buffer[offset], "USSID FATHERSHIP", 16);
445|    offset += 16;
446|    
447|    // Supported Rates (Tag 1)
448|    buffer[offset++] = 0x01;  // Tag: Supported Rates
449|    buffer[offset++] = 0x08;  // Length: 8
450|    buffer[offset++] = 0x82;  // 1 Mbps (basic)
451|    buffer[offset++] = 0x84;  // 2 Mbps (basic)
452|    buffer[offset++] = 0x8B;  // 5.5 Mbps (basic)
453|    buffer[offset++] = 0x96;  // 11 Mbps (basic)
454|    buffer[offset++] = 0x0C;  // 6 Mbps
455|    buffer[offset++] = 0x12;  // 9 Mbps
456|    buffer[offset++] = 0x18;  // 12 Mbps
457|    buffer[offset++] = 0x24;  // 18 Mbps
458|    
459|    // DS Parameter Set (Tag 3)
460|    buffer[offset++] = 0x03;  // Tag: DS Parameter Set
461|    buffer[offset++] = 0x01;  // Length: 1
462|    buffer[offset++] = BACON_CHANNEL;
463|    
464|    // Vendor Specific IE (our AP fingerprint)
465|    if (apCount > 0) {
466|        // Ensure vendor IE fits in the remaining buffer
467|        size_t remaining = (offset < maxLen) ? (maxLen - offset) : 0;
468|        uint8_t maxAps = 0;
469|        if (remaining > 7) {
470|            size_t maxBySpace = (remaining - 7) / 8;
471|            if (maxBySpace > 255) maxBySpace = 255;
472|            maxAps = (uint8_t)maxBySpace;
473|        }
474|        uint8_t safeCount = apCount;
475|        if (safeCount > maxAps) safeCount = maxAps;
476|        if (safeCount == 0) {
477|            *len = offset;
478|            return;
479|        }
480|        size_t vendorLen = 0;
481|        buildVendorIE(&buffer[offset], &vendorLen, safeCount);
482|        offset += vendorLen;
483|    }
484|    
485|    *len = offset;
486|}
487|
488|void BaconMode::sendBeacon() {
489|    uint8_t beaconFrame[256];
490|    size_t frameLen = 0;
491|    
492|    // Build beacon frame
493|    buildBeaconFrame(beaconFrame, &frameLen);
494|    
495|    // Transmit
496|    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, beaconFrame, frameLen, false);
497|    
498|    if (err != ESP_OK) {
499|        Serial.printf("[BACON] Beacon TX failed: %d\n", err);
500|    }
501|