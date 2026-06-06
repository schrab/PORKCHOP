1|// Warhog Mode implementation - Refactored "GPS as Gate" architecture
2|// 
3|// Key changes from original:
4|// - No entries[] vector - data goes directly to disk
5|// - No "waiting for GPS" state - either GPS or ML-only
6|// - Simpler memory management - Bloom filter for duplicate detection
7|// - Per-network file writes instead of batch saves
8|
9|#include "warhog.h"
10|#include "oink.h"
11|#include "../build_info.h"
12|#include "../core/config.h"
13|#include "../core/wifi_utils.h"
14|#include "../core/heap_policy.h"
15|#include "../core/heap_health.h"
16|#include "../core/network_recon.h"
17|#include "../core/wsl_bypasser.h"
18|#include "../core/sdlog.h"
19|#include "../core/sd_layout.h"
20|#include "../core/xp.h"
21|#include "../ui/display.h"
22|#include "../piglet/mood.h"
23|#include "../piglet/avatar.h"
24|// No M5Cardputer on ESP32-S3 Mini
25|#include <WiFi.h>
26|#include <SD.h>
27|#include <freertos/FreeRTOS.h>
28|#include <freertos/task.h>
29|#include <math.h>
30|#include <string.h>
31|#include <esp_heap_caps.h>
32|
33|// Bloom filter for seen BSSIDs (fixed memory, no heap churn)
34|// 4KB = 32,768 bits -> ~1.5% false positives around 5k entries with 3 hashes
35|static const size_t SEEN_BLOOM_BYTES = 4096;
36|static const size_t SEEN_BLOOM_BITS = SEEN_BLOOM_BYTES * 8;
37|static const size_t SEEN_BLOOM_MASK = SEEN_BLOOM_BITS - 1;
38|static const uint8_t SEEN_BLOOM_HASHES = 3;
39|static_assert((SEEN_BLOOM_BITS & (SEEN_BLOOM_BITS - 1)) == 0, "SEEN_BLOOM_BITS must be power of two");
40|
41|// Captured bloom for bounty exclusion (small, fast)
42|static const size_t CAPTURED_BLOOM_BYTES = 2048;
43|static const size_t CAPTURED_BLOOM_BITS = CAPTURED_BLOOM_BYTES * 8;
44|static const size_t CAPTURED_BLOOM_MASK = CAPTURED_BLOOM_BITS - 1;
45|static const uint8_t CAPTURED_BLOOM_HASHES = 3;
46|static_assert((CAPTURED_BLOOM_BITS & (CAPTURED_BLOOM_BITS - 1)) == 0, "CAPTURED_BLOOM_BITS must be power of two");
47|
48|// Bounty pool (reservoir sample of seen networks)
49|static const size_t BOUNTY_POOL_SIZE = 50;
50|
51|// Heap threshold for emergency cleanup (bytes) - centralized in HeapPolicy
52|
53|// Minimum scan interval to avoid tight-loop scanning
54|static const uint32_t SCAN_INTERVAL_MIN_MS = 1000;
55|
56|// SD card retry settings (SD can be busy with other operations)
57|static const int SD_RETRY_COUNT = 3;
58|static const int SD_RETRY_DELAY_MS = 10;
59|
60|// WiGLE file size limit for upload compatibility (400KB - leave room for headers)
61|// Files larger than this will be rotated to a new file
62|static const size_t WIGLE_FILE_MAX_SIZE = 400000;
63|
64|// Graceful stop request flag for background scan task
65|static volatile bool stopRequested = false;
66|// Set by scan task just before self-deleting, used for safe cleanup in stop()
67|static volatile bool scanTaskExited = false;
68|
69|// Helper: Open SD file with retry logic
70|static File openFileWithRetry(const char* path, const char* mode) {
71|    File f;
72|    for (int retry = 0; retry < SD_RETRY_COUNT; retry++) {
73|        f = SD.open(path, mode);
74|        if (f) return f;
75|        delay(SD_RETRY_DELAY_MS);
76|    }
77|    return f;  // Returns invalid File if all retries failed
78|}
79|
80|// Haversine formula for GPS distance calculation
81|static double haversineMeters(double lat1, double lon1, double lat2, double lon2) {
82|    const double R = 6371000.0;  // Earth radius in meters
83|    double dLat = (lat2 - lat1) * M_PI / 180.0;
84|    double dLon = (lon2 - lon1) * M_PI / 180.0;
85|    lat1 = lat1 * M_PI / 180.0;
86|    lat2 = lat2 * M_PI / 180.0;
87|    
88|    double a = sin(dLat / 2) * sin(dLat / 2) +
89|               cos(lat1) * cos(lat2) * sin(dLon / 2) * sin(dLon / 2);
90|    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
91|    return R * c;
92|}
93|
94|// Distance tracking state
95|static double lastGPSLat = 0;
96|static double lastGPSLon = 0;
97|static uint32_t lastDistanceCheck = 0;
98|
99|// Static members
100|bool WarhogMode::running = false;
101|uint32_t WarhogMode::lastScanTime = 0;
102|uint32_t WarhogMode::scanInterval = 5000;
103|static uint8_t seenBloom[SEEN_BLOOM_BYTES];
104|static uint8_t capturedBloom[CAPTURED_BLOOM_BYTES];
105|static uint64_t bountyPool[BOUNTY_POOL_SIZE];
106|static uint16_t bountyPoolCount = 0;
107|static uint32_t bountySeenTotal = 0;
108|uint32_t WarhogMode::totalNetworks = 0;
109|uint32_t WarhogMode::openNetworks = 0;
110|uint32_t WarhogMode::wepNetworks = 0;
111|uint32_t WarhogMode::wpaNetworks = 0;
112|uint32_t WarhogMode::savedCount = 0;      // Geotagged networks (CSV)
113|char WarhogMode::currentFilename[128] = {0};
114|char WarhogMode::currentWigleFilename[128] = {0};
115|
116|// Scan state
117|bool WarhogMode::scanInProgress = false;
118|uint32_t WarhogMode::scanStartTime = 0;
119|
120|// Background scan task statics
121|TaskHandle_t WarhogMode::scanTaskHandle = NULL;
122|volatile int WarhogMode::scanResult = -2;  // -2 = not started, -1 = running, >=0 = complete
123|
124|// Scan task check: returns true if should abort
125|static inline bool shouldAbortScan() {
126|    return stopRequested || !WarhogMode::isRunning();
127|}
128|
129|static uint32_t mix32(uint64_t x) {
130|    x ^= x >> 33;
131|    x *= 0xff51afd7ed558ccdULL;
132|    x ^= x >> 33;
133|    x *= 0xc4ceb9fe1a85ec53ULL;
134|    x ^= x >> 33;
135|    return (uint32_t)x;
136|}
137|
138|static bool bloomTest(const uint8_t* bloom, size_t mask, uint8_t hashes, uint64_t key) {
139|    uint32_t h1 = mix32(key);
140|    uint32_t h2 = mix32(key ^ 0x9e3779b97f4a7c15ULL) | 1U;
141|    for (uint8_t i = 0; i < hashes; i++) {
142|        uint32_t idx = (h1 + (uint32_t)i * h2) & (uint32_t)mask;
143|        if ((bloom[idx >> 3] & (1 << (idx & 7))) == 0) {
144|            return false;
145|        }
146|    }
147|    return true;
148|}
149|
150|static void bloomAdd(uint8_t* bloom, size_t mask, uint8_t hashes, uint64_t key) {
151|    uint32_t h1 = mix32(key);
152|    uint32_t h2 = mix32(key ^ 0x9e3779b97f4a7c15ULL) | 1U;
153|    for (uint8_t i = 0; i < hashes; i++) {
154|        uint32_t idx = (h1 + (uint32_t)i * h2) & (uint32_t)mask;
155|        bloom[idx >> 3] |= (1 << (idx & 7));
156|    }
157|}
158|
159|static void resetSeenTracking() {
160|    memset(seenBloom, 0, sizeof(seenBloom));
161|    memset(capturedBloom, 0, sizeof(capturedBloom));
162|    bountyPoolCount = 0;
163|    bountySeenTotal = 0;
164|}
165|
166|static void seedCapturedFromOink() {
167|    for (const auto& hs : OinkMode::getHandshakes()) {
168|        bloomAdd(capturedBloom, CAPTURED_BLOOM_MASK, CAPTURED_BLOOM_HASHES, bssidToKey(hs.bssid));
169|    }
170|    for (const auto& p : OinkMode::getPMKIDs()) {
171|        bloomAdd(capturedBloom, CAPTURED_BLOOM_MASK, CAPTURED_BLOOM_HASHES, bssidToKey(p.bssid));
172|    }
173|}
174|
175|static uint32_t clampScanIntervalMs(uint32_t intervalMs) {
176|    return (intervalMs < SCAN_INTERVAL_MIN_MS) ? SCAN_INTERVAL_MIN_MS : intervalMs;
177|}
178|
179|// Helper to write CSV-escaped SSID field (quoted, doubles internal quotes, strips control chars)
180|static void writeCSVField(File& f, const char* ssid) {
181|    f.print("\"");
182|    for (int i = 0; i < 32 && ssid[i]; i++) {
183|        if (ssid[i] == '"') {
184|            f.print("\"\"");
185|        } else if (ssid[i] >= 32) {  // Skip control characters (newlines, etc)
186|            f.print(ssid[i]);
187|        }
188|    }
189|    f.print("\"");
190|}
191|
192|void WarhogMode::init() {
193|    totalNetworks = 0;
194|    openNetworks = 0;
195|    wepNetworks = 0;
196|    wpaNetworks = 0;
197|    savedCount = 0;
198|    currentFilename[0] = '\0';
199|    currentWigleFilename[0] = '\0';
200|
201|    resetSeenTracking();
202|
203|    scanInterval = clampScanIntervalMs(Config::gps().updateInterval * 1000UL);
204|}
205|
206|void WarhogMode::start() {
207|    if (running) return;
208|
209|    // Clear previous session data
210|    totalNetworks = 0;
211|    openNetworks = 0;
212|    wepNetworks = 0;
213|    wpaNetworks = 0;
214|    savedCount = 0;
215|    currentFilename[0] = '\0';
216|    currentWigleFilename[0] = '\0';
217|
218|    resetSeenTracking();
219|    seedCapturedFromOink();
220|
221|    // Reset distance tracking for XP
222|    lastGPSLat = 0;
223|    lastGPSLon = 0;
224|    lastDistanceCheck = 0;
225|    
226|    // Reload scan interval from config
227|    scanInterval = clampScanIntervalMs(Config::gps().updateInterval * 1000UL);
228|    
229|    // Reset stop flag for clean start
230|    stopRequested = false;
231|
232|    // Stop NetworkRecon before WiFi manipulation (uses promiscuous mode, incompatible with STA scanning)
233|    NetworkRecon::stop();
234|    
235|    // Soft WiFi reset — keep driver alive to avoid esp_wifi_init() RX buffer failures
236|    WiFi.disconnect(false, true);  // Keep driver, erase AP credentials
237|    delay(200);             // Let it settle
238|    WiFi.mode(WIFI_STA);    // Station mode for scanning
239|    
240|    // Randomize MAC if enabled (stealth)
241|    if (Config::wifi().randomizeMAC) {
242|        WSLBypasser::randomizeMAC();
243|    }
244|    
245|    delay(200);             // Let it initialize
246|    
247|    // Reset scan state (critical for proper operation after restart)
248|    scanInProgress = false;
249|    scanStartTime = 0;
250|
251|    // Ensure GPS is in continuous mode regardless of software state
252|    // FIX: Addresses issue where GPS doesn't show until mode restart
253|    GPS::ensureContinuousMode();
254|    
255|    running = true;
256|    lastScanTime = 0;  // Trigger immediate scan
257|    
258|    // Set grass speed for wardriving - animation controlled by GPS lock in update()
259|    Avatar::setGrassSpeed(200);  // Slower than OINK (~5 FPS)
260|    Avatar::setGrassMoving(GPS::hasFix());  // Start based on current GPS status
261|    
262|    Display::setWiFiStatus(true);
263|    Mood::onWarhogUpdate();  // Show WARHOG phrase on start
264|    Mood::setDialogueLock(true);
265|}
266|
267|
268|void WarhogMode::stop() {
269|    if (!running) return;
270|    
271|    // Signal task to stop gracefully
272|    stopRequested = true;
273|    scanTaskExited = false;
274|
275|    // Wait briefly for background scan to notice stopRequested
276|    if (scanInProgress && scanTaskHandle != NULL) {
277|        // Give task up to 500ms to exit gracefully
278|        for (int i = 0; i < 10 && scanTaskHandle != NULL; i++) {
279|            delay(50);
280|        }
281|        // Force cleanup if task didn't exit in time
282|        if (scanTaskHandle != NULL) {
283|            Serial.println("[WARHOG] Force-deleting scan task");
284|            vTaskDelete(scanTaskHandle);
285|            scanTaskHandle = NULL;
286|        }
287|        // Only call scanDelete if task exited cleanly (not mid-scan-processing)
288|        if (scanTaskExited) {
289|            WiFi.scanDelete();
290|        } else {
291|            // Task was force-killed — WiFi state may be inconsistent.
292|            // Soft reset keeps driver alive (avoid RX buffer realloc on fragmented heap).
293|            WiFi.disconnect(false, true);
294|            delay(50);
295|        }
296|    }
297|    scanInProgress = false;
298|    scanResult = -2;
299|    
300|    // Stop grass animation
301|    Avatar::setGrassMoving(false);
302|    
303|    running = false;
304|    
305|    // Put GPS to sleep if power management enabled
306|    if (Config::gps().powerSave) {
307|        GPS::sleep();
308|    }
309|    
310|    // Restart NetworkRecon (restores promiscuous mode for OINK/DNH/etc)
311|    NetworkRecon::start();
312|    Display::setWiFiStatus(true);  // Recon is active
313|    
314|    // Reset stop flag for next run
315|    stopRequested = false;
316|    Mood::setDialogueLock(false);
317|}
318|
319|// Background task for WiFi scanning - runs sync scan without blocking main loop
320|void WarhogMode::scanTask(void* pvParameters) {
321|    // Check for early abort request
322|    if (shouldAbortScan()) {
323|        scanResult = -2;
324|        scanTaskExited = true;
325|        scanTaskHandle = NULL;
326|        vTaskDelete(NULL);
327|        return;
328|    }
329|
330|    // Soft WiFi reset — keep driver alive to avoid RX buffer realloc failures
331|    WiFi.scanDelete();
332|    WiFi.disconnect(false, true);
333|        vTaskDelay(pdMS_TO_TICKS(100));
334|
335|    // Check abort between WiFi operations
336|    if (shouldAbortScan()) {
337|        scanResult = -2;
338|        scanTaskExited = true;
339|        scanTaskHandle = NULL;
340|        vTaskDelete(NULL);
341|        return;
342|    }
343|    
344|    WiFi.mode(WIFI_STA);
345|    vTaskDelay(pdMS_TO_TICKS(100));
346|    
347|    // Sync scan - this blocks until complete (which is fine in background task)
348|    int result = WiFi.scanNetworks(false, true);  // sync, show hidden
349|    
350|    // Store result for main loop to pick up
351|    scanResult = result;
352|
353|    // Log stack usage for sizing decisions
354|    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
355|    Serial.printf("[WARHOG] Scan task stack HWM: %u bytes unused of 4096\n",
356|                  (unsigned)(hwm * sizeof(StackType_t)));
357|
358|    // Signal clean exit, then self-delete
359|    scanTaskExited = true;
360|    scanTaskHandle = NULL;
361|    vTaskDelete(NULL);
362|}
363|
364|void WarhogMode::update() {
365|    if (!running) return;
366|    
367|    uint32_t now = millis();
368|    static uint32_t lastPhraseTime = 0;
369|    static bool lastGPSState = false;
370|    static uint32_t lastHeapCheck = 0;
371|    
372|    // Periodic heap monitoring (every 30 seconds)
373|    if (now - lastHeapCheck >= 30000) {
374|        if (HeapHealth::getPressureLevel() >= HeapPressureLevel::Critical) {
375|            Display::showToast("LOW MEMORY!");
376|        }
377|        lastHeapCheck = now;
378|    }
379|
380|    // Update grass animation based on GPS fix status
381|    bool hasGPSFix = GPS::hasFix();
382|    if (hasGPSFix != lastGPSState) {
383|        Avatar::setGrassMoving(hasGPSFix);
384|        lastGPSState = hasGPSFix;
385|    }
386|    
387|    // Distance tracking for XP (every 5 seconds when GPS is available)
388|    if (hasGPSFix && now - lastDistanceCheck >= 5000) {
389|        GPSData gps = GPS::getData();
390|        if (lastGPSLat != 0 && lastGPSLon != 0) {
391|            double distance = haversineMeters(lastGPSLat, lastGPSLon, gps.latitude, gps.longitude);
392|            // Filter out GPS jitter (<5m) and teleportation (>1km)
393|            if (distance > 5.0 && distance < 1000.0) {
394|                XP::addDistance((uint32_t)distance);
395|            }
396|        }
397|        lastGPSLat = gps.latitude;
398|        lastGPSLon = gps.longitude;
399|        lastDistanceCheck = now;
400|    }
401|    
402|    // Rotate phrases every 5 seconds when idle
403|    if (now - lastPhraseTime >= 5000) {
404|        Mood::onWarhogUpdate();
405|        lastPhraseTime = now;
406|    }
407|    
408|    // Check if background scan task is complete
409|    if (scanInProgress) {
410|        if (scanResult >= 0) {
411|            // Scan done
412|            scanInProgress = false;
413|            processScanResults();
414|            scanResult = -2;  // Reset for next scan
415|        } else if (scanTaskHandle == NULL && scanResult == -2) {
416|            // Task ended but no result - something went wrong
417|            scanInProgress = false;
418|        } else if (now - scanStartTime > 20000) {
419|            // Timeout after 20 seconds
420|            if (scanTaskHandle != NULL) {
421|                vTaskDelete(scanTaskHandle);
422|                scanTaskHandle = NULL;
423|            }
424|            scanInProgress = false;
425|            scanResult = -2;
426|            WiFi.scanDelete();
427|        }
428|        // Still running - just return (UI stays responsive)
429|        return;
430|    }
431|    
432|    // Start new scan if interval elapsed and not already scanning
433|    if (now - lastScanTime >= scanInterval) {
434|        performScan();
435|        lastScanTime = now;
436|    }
437|}
438|
439|void WarhogMode::triggerScan() {
440|    if (!scanInProgress) {
441|        performScan();
442|    }
443|}
444|
445|bool WarhogMode::isScanComplete() {
446|    return !scanInProgress && scanResult >= 0;
447|}
448|
449|void WarhogMode::performScan() {
450|    if (scanInProgress) return;
451|    if (scanTaskHandle != NULL) return;  // Previous task still running
452|
453|    scanInProgress = true;
454|    scanStartTime = millis();
455|    scanResult = -1;  // Running
456|    scanTaskExited = false;
457|
458|    // Create background task for sync scan
459|    xTaskCreatePinnedToCore(
460|        scanTask,           // Function
461|        "wifiScan",         // Name
462|        4096,               // Stack size
463|        NULL,               // Parameters
464|        1,                  // Priority (low)
465|        &scanTaskHandle,    // Task handle
466|        0                   // Run on core 0 (WiFi core)
467|    );
468|    
469|    if (scanTaskHandle == NULL) {
470|        // Fallback: run sync scan on main thread if task creation fails
471|        scanInProgress = false;
472|        scanResult = WiFi.scanNetworks(false, true);
473|        if (scanResult >= 0) {
474|            processScanResults();
475|        }
476|        scanResult = -2;
477|    }
478|}
479|
480|// Ensure CSV file exists with header
481|bool WarhogMode::ensureCSVFileReady() {
482|    if (currentFilename[0] != '\0') return true;
483|
484|    // Ensure wardriving directory exists
485|    const char* wardrivingDir = SDLayout::wardrivingDir();
486|    if (!SD.exists(wardrivingDir)) {
487|        if (!SD.mkdir(wardrivingDir)) {
488|            return false;
489|        }
490|    }
491|
492|    generateFilename(currentFilename, sizeof(currentFilename), "csv");
493|
494|    File f = openFileWithRetry(currentFilename, FILE_WRITE);
495|    if (!f) {
496|        currentFilename[0] = '\0';
497|        return false;
498|    }
499|    
500|    f.println("BSSID,SSID,RSSI,Channel,AuthMode,Latitude,Longitude,Altitude,Timestamp");
501|