1|// Captures Menu - View saved handshake captures
2|
3|#include "captures_menu.h"
4|#include "../hal/hal_input.h"
#include "../hal/hal_display.h"
5|#include <SD.h>
6|#include <WiFi.h>
7|#include <time.h>
8|#include <ctype.h>
9|#include <string.h>
10|#include "display.h"
11|#include "../web/wpasec.h"
12|#include "../core/config.h"
13|#include "../core/sd_layout.h"
14|#include "../core/wifi_utils.h"
15|#include "../core/heap_health.h"
16|#include <esp_heap_caps.h>
17|
18|// Static member initialization
19|std::vector<CaptureInfo> CapturesMenu::captures;
20|uint8_t CapturesMenu::selectedIndex = 0;
21|uint8_t CapturesMenu::scrollOffset = 0;
22|bool CapturesMenu::active = false;
23|bool CapturesMenu::keyWasPressed = false;
24|bool CapturesMenu::nukeConfirmActive = false;
25|bool CapturesMenu::detailViewActive = false;
26|bool CapturesMenu::scanInProgress = false;
27|unsigned long CapturesMenu::lastScanTime = 0;
28|File CapturesMenu::scanDir;
29|File CapturesMenu::currentFile;
30|bool CapturesMenu::scanComplete = false;
31|size_t CapturesMenu::scanProgress = 0;
32|bool CapturesMenu::wpasecUpdateInProgress = false;
33|unsigned long CapturesMenu::lastWpasecUpdateTime = 0;
34|size_t CapturesMenu::wpasecUpdateProgress = 0;
35|
36|// Hint rotation
37|uint8_t CapturesMenu::hintIndex = 0;
38|const char* const CapturesMenu::HINTS[] = {
39|    "FEED YO HASHCAT.",
40|    "COLLECTED PAIN. COMPRESSED.",
41|    "ENT:DET  S:SYNC  D:NUKE",
42|    "MALLOC SAID NAH.",
43|    "YOUR LOOT. YOUR PROBLEM."
44|};
45|
46|// WPA-SEC Sync state
47|bool CapturesMenu::syncModalActive = false;
48|SyncState CapturesMenu::syncState = SyncState::IDLE;
49|char CapturesMenu::syncStatusText[48] = "";
50|uint8_t CapturesMenu::syncProgress = 0;
51|uint8_t CapturesMenu::syncTotal = 0;
52|unsigned long CapturesMenu::syncStartTime = 0;
53|uint8_t CapturesMenu::syncUploaded = 0;
54|uint8_t CapturesMenu::syncFailed = 0;
55|uint16_t CapturesMenu::syncCracked = 0;
56|char CapturesMenu::syncError[48] = "";
57|
58|void CapturesMenu::init() {
59|    captures.clear();
60|    selectedIndex = 0;
61|    scrollOffset = 0;
62|}
63|
64|void CapturesMenu::show() {
65|    active = true;
66|    selectedIndex = 0;
67|    scrollOffset = 0;
68|    keyWasPressed = true;  // Ignore the Enter that selected us from menu
69|    hintIndex = esp_random() % HINT_COUNT;
70|
71|    // If scan fails, the captures list will remain empty
72|    // This is handled by the draw function which shows "No captures found"
73|    scanCaptures();
74|}
75|
76|void CapturesMenu::hide() {
77|    active = false;
78|    
79|    // FIX: Always call emergencyCleanup first - ensures file handles closed
80|    emergencyCleanup();
81|    
82|    // Enhanced: Force cleanup even if interrupted
83|    captures.clear();
84|    captures.shrink_to_fit();  // Release vector capacity
85|    WPASec::freeCacheMemory();
86|    
87|    // Reset all async state to prevent leaks (redundant after emergencyCleanup but safe)
88|    scanInProgress = false;
89|    wpasecUpdateInProgress = false;
90|    if (scanDir) {
91|        scanDir.close();
92|    }
93|    if (currentFile) {
94|        currentFile.close();
95|    }
96|}
97|
98|void CapturesMenu::emergencyCleanup() {
99|    // Can be called from main loop when heap is critical
100|    if (!active) return;
101|    
102|    Serial.println("[CAPTURES] Emergency cleanup triggered");
103|    captures.clear();
104|    captures.shrink_to_fit();
105|    WPASec::freeCacheMemory();
106|    
107|    // Stop any in-progress operations
108|    scanInProgress = false;
109|    wpasecUpdateInProgress = false;
110|    if (scanDir) {
111|        scanDir.close();
112|    }
113|    if (currentFile) {
114|        currentFile.close();
115|    }
116|}
117|
118|bool CapturesMenu::scanCaptures() {
119|    // Initialize async scan
120|    captures.clear();
121|    captures.reserve(MAX_CAPTURES);  // Full upfront reserve — no mid-scan reallocations
122|
123|    // Guard: Skip if no SD card available
124|    if (!Config::isSDAvailable()) {
125|        Serial.println("[CAPTURES] No SD card available");
126|        scanComplete = true;
127|        scanInProgress = false;
128|        return false;
129|    }
130|
131|    // Guard: Skip SD scan at Warning+ pressure — file ops allocate FAT buffers
132|    if (HeapHealth::getPressureLevel() >= HeapPressureLevel::Warning) {
133|        Serial.println("[CAPTURES] Scan deferred: heap pressure");
134|        scanComplete = true;
135|        scanInProgress = false;
136|        return false;
137|    }
138|
139|    // Create directory if it doesn't exist
140|    const char* handshakesDir = SDLayout::handshakesDir();
141|    if (!SD.exists(handshakesDir)) {
142|        Serial.println("[CAPTURES] No handshakes directory, creating...");
143|        if (!SD.mkdir(handshakesDir)) {
144|            Serial.println("[CAPTURES] Failed to create handshakes directory");
145|            scanComplete = true;
146|            scanInProgress = false;
147|            return false;
148|        }
149|    }
150|
151|    scanDir = SD.open(handshakesDir);
152|    if (!scanDir || !scanDir.isDirectory()) {
153|        Serial.println("[CAPTURES] Failed to open handshakes directory");
154|        scanComplete = true;
155|        scanInProgress = false;
156|        scanDir.close();
157|        return false;
158|    }
159|
160|    scanInProgress = true;
161|    scanComplete = false;
162|    scanProgress = 0;
163|    lastScanTime = millis();
164|    
165|    return true;
166|}
167|
168|// Helper: check if string of length n is all hex chars
169|static bool isAllHex(const char* s, size_t n) {
170|    for (size_t i = 0; i < n; i++) {
171|        char c = s[i];
172|        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
173|            return false;
174|    }
175|    return true;
176|}
177|
178|void CapturesMenu::processAsyncScan() {
179|    if (!scanInProgress || scanComplete) {
180|        return;
181|    }
182|
183|    // Throttle the scan to avoid blocking the UI
184|    if (millis() - lastScanTime < SCAN_DELAY) {
185|        return;
186|    }
187|
188|    lastScanTime = millis();
189|
190|    // Process a chunk of files
191|    size_t processed = 0;
192|    while (processed < SCAN_CHUNK_SIZE && !scanComplete) {
193|        currentFile = scanDir.openNextFile();
194|
195|        if (!currentFile) {
196|            // No more files, we're done with scanning
197|            scanComplete = true;
198|            scanInProgress = false;
199|            scanDir.close();
200|
201|            // Sort by capture time (newest first)
202|            std::sort(captures.begin(), captures.end(), [](const CaptureInfo& a, const CaptureInfo& b) {
203|                return a.captureTime > b.captureTime;
204|            });
205|
206|            // Start async WPA-SEC status update after scanning is complete
207|            if (!captures.empty()) {
208|                wpasecUpdateInProgress = true;
209|                wpasecUpdateProgress = 0;
210|                lastWpasecUpdateTime = millis();
211|            }
212|
213|            Serial.printf("[CAPTURES] Async scan complete. Found %d captures\n", captures.size());
214|            break;
215|        }
216|
217|        // Zero-String scan: use const char* from File directly
218|        const char* name = currentFile.name();
219|        size_t nameLen = strlen(name);
220|
221|        bool isPCAP = (nameLen > 5 && strcmp(name + nameLen - 5, ".pcap") == 0);
222|        bool isHS22000 = (nameLen > 9 && strcmp(name + nameLen - 9, "_hs.22000") == 0);
223|        bool isPMKID = !isHS22000 && (nameLen > 6 && strcmp(name + nameLen - 6, ".22000") == 0);
224|
225|        // Skip PCAP if we have the corresponding _hs.22000 (avoid duplicates).
226|        if (isPCAP) {
227|            // Build base name: everything before the dot
228|            const char* dot = strrchr(name, '.');
229|            size_t baseLen = dot ? (size_t)(dot - name) : nameLen;
230|            char hs22kPath[80];
231|            snprintf(hs22kPath, sizeof(hs22kPath), "%s/%.*s_hs.22000",
232|                     SDLayout::handshakesDir(), (int)baseLen, name);
233|            if (SD.exists(hs22kPath)) {
234|                currentFile.close();
235|                processed++;
236|                continue;
237|            }
238|        }
239|
240|        if (isPCAP || isPMKID || isHS22000) {
241|            CaptureInfo info;
242|            memset(&info, 0, sizeof(info));
243|            strncpy(info.filename, name, sizeof(info.filename) - 1);
244|            info.fileSize = currentFile.size();
245|            info.captureTime = currentFile.getLastWrite();
246|            info.isPMKID = isPMKID;
247|
248|            // Compute base name (strip extension and _hs suffix)
249|            const char* dot = strrchr(name, '.');
250|            size_t baseLen = dot ? (size_t)(dot - name) : nameLen;
251|            if (baseLen > 3 && strncmp(name + baseLen - 3, "_hs", 3) == 0) {
252|                baseLen -= 3;
253|            }
254|
255|            // Dual-format detection:
256|            // Legacy: base name is exactly 12 hex chars (BSSID only)
257|            // New format: last 12 chars are hex, preceded by '_' (SSID_BSSID)
258|            if (baseLen == 12 && isAllHex(name, 12)) {
259|                // Legacy format: BSSID is first 12 chars
260|                const char* b = name;
261|                snprintf(info.bssid, sizeof(info.bssid),
262|                         "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s",
263|                         b, b+2, b+4, b+6, b+8, b+10);
264|
265|                // Try companion .txt for SSID (legacy files)
266|                char txtPath[80];
267|                if (isPMKID) {
268|                    snprintf(txtPath, sizeof(txtPath), "%s/%.12s_pmkid.txt",
269|                             SDLayout::handshakesDir(), name);
270|                } else {
271|                    snprintf(txtPath, sizeof(txtPath), "%s/%.12s.txt",
272|                             SDLayout::handshakesDir(), name);
273|                }
274|                if (SD.exists(txtPath)) {
275|                    File txtFile = SD.open(txtPath, FILE_READ);
276|                    if (txtFile) {
277|                        char buf[34];
278|                        int n = txtFile.readBytesUntil('\n', buf, sizeof(buf) - 1);
279|                        buf[n] = '\0';
280|                        while (n > 0 && (buf[n-1] == ' ' || buf[n-1] == '\r' || buf[n-1] == '\t')) buf[--n] = '\0';
281|                        if (n > 0) {
282|                            strncpy(info.ssid, buf, sizeof(info.ssid) - 1);
283|                        }
284|                        txtFile.close();
285|                    }
286|                }
287|            } else if (baseLen > 13 && name[baseLen - 13] == '_' &&
288|                       isAllHex(name + baseLen - 12, 12)) {
289|                // New format: SSID_BSSID — extract BSSID from last 12 chars
290|                const char* b = name + baseLen - 12;
291|                snprintf(info.bssid, sizeof(info.bssid),
292|                         "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s",
293|                         b, b+2, b+4, b+6, b+8, b+10);
294|
295|                // Extract SSID from chars before _BSSID
296|                size_t ssidLen = baseLen - 13;
297|                if (ssidLen > sizeof(info.ssid) - 1) ssidLen = sizeof(info.ssid) - 1;
298|                memcpy(info.ssid, name, ssidLen);
299|                info.ssid[ssidLen] = '\0';
300|            } else {
301|                // Unknown format — use full base as BSSID display
302|                size_t copyLen = baseLen < sizeof(info.bssid) - 1 ? baseLen : sizeof(info.bssid) - 1;
303|                memcpy(info.bssid, name, copyLen);
304|                info.bssid[copyLen] = '\0';
305|            }
306|
307|            if (info.ssid[0] == '\0') {
308|                strncpy(info.ssid, "[UNKNOWN]", sizeof(info.ssid) - 1);
309|            }
310|
311|            info.status = CaptureStatus::LOCAL;
312|
313|            captures.push_back(info);
314|
315|            if (captures.size() >= MAX_CAPTURES) {
316|                scanComplete = true;
317|                scanInProgress = false;
318|                currentFile.close();
319|                scanDir.close();
320|                Serial.println("[CAPTURES] Hit capture limit, stopped scan");
321|                break;
322|            }
323|        }
324|
325|        currentFile.close();
326|        processed++;
327|        scanProgress++;
328|
329|        if (processed >= SCAN_CHUNK_SIZE) {
330|            break;
331|        }
332|    }
333|}
334|
335|void CapturesMenu::updateWPASecStatus() {
336|    // Load WPA-SEC cache (lazy, only loads once)
337|    WPASec::loadCache();
338|    
339|    char normalized[13] = {0};
340|    for (auto& cap : captures) {
341|        // Normalize BSSID for lookup (remove colons)
342|        WPASec::normalizeBSSID_Char(cap.bssid, normalized, sizeof(normalized));
343|        if (normalized[0] == '\0') {
344|            cap.status = CaptureStatus::LOCAL;
345|            continue;
346|        }
347|        
348|        if (WPASec::isCracked(normalized)) {
349|            cap.status = CaptureStatus::CRACKED;
350|            strncpy(cap.password, WPASec::getPassword(normalized), sizeof(cap.password) - 1);
351|            cap.password[sizeof(cap.password) - 1] = '\0';
352|        } else if (WPASec::isUploaded(normalized)) {
353|            cap.status = CaptureStatus::UPLOADED;
354|        } else {
355|            cap.status = CaptureStatus::LOCAL;
356|        }
357|    }
358|}
359|
360|void CapturesMenu::processAsyncWPASecUpdate() {
361|    if (!wpasecUpdateInProgress || captures.empty()) {
362|        wpasecUpdateInProgress = false;
363|        return;
364|    }
365|    
366|    // Throttle the update to avoid blocking the UI
367|    if (millis() - lastWpasecUpdateTime < WPASEC_UPDATE_DELAY) {
368|        return;
369|    }
370|    
371|    lastWpasecUpdateTime = millis();
372|    
373|    // Process a chunk of captures
374|    size_t processed = 0;
375|    while (processed < WPASEC_UPDATE_CHUNK_SIZE && wpasecUpdateProgress < captures.size()) {
376|        auto& cap = captures[wpasecUpdateProgress];
377|        
378|        // Normalize BSSID for lookup (remove colons)
379|        char normalized[13] = {0};
380|        WPASec::normalizeBSSID_Char(cap.bssid, normalized, sizeof(normalized));
381|        
382|        if (normalized[0] != '\0') {
383|            if (WPASec::isCracked(normalized)) {
384|                cap.status = CaptureStatus::CRACKED;
385|                strncpy(cap.password, WPASec::getPassword(normalized), sizeof(cap.password) - 1);
386|                cap.password[sizeof(cap.password) - 1] = '\0';
387|            } else if (WPASec::isUploaded(normalized)) {
388|                cap.status = CaptureStatus::UPLOADED;
389|            } else {
390|                cap.status = CaptureStatus::LOCAL;
391|            }
392|        } else {
393|            cap.status = CaptureStatus::LOCAL;
394|        }
395|        
396|        wpasecUpdateProgress++;
397|        processed++;
398|        
399|        // Yield periodically to allow other tasks to run
400|        if (processed >= WPASEC_UPDATE_CHUNK_SIZE) {
401|            // Still more to do, but yield control back to other tasks
402|            break;
403|        }
404|    }
405|    
406|    // Check if we're done with all captures
407|    if (wpasecUpdateProgress >= captures.size()) {
408|        wpasecUpdateInProgress = false;
409|        Serial.printf("[CAPTURES] Async WPA-SEC update complete. Updated %d captures\n", captures.size());
410|    }
411|}
412|
413|void CapturesMenu::update() {
414|    if (!active) return;
415|    
416|    // Process sync state machine if active
417|    if (syncModalActive && syncState != SyncState::IDLE && 
418|        syncState != SyncState::COMPLETE && syncState != SyncState::ERROR) {
419|        processSyncState();
420|    }
421|    
422|    // Process async file scanning if in progress (not during sync)
423|    if (!syncModalActive) {
424|        processAsyncScan();
425|        
426|        // Process async WPA-SEC status updates if in progress
427|        processAsyncWPASecUpdate();
428|    }
429|    
430|    handleInput();
431|}
432|
433|void CapturesMenu::handleInput() {
434|    bool anyPressed = hal_input_anyHeld();
435|    
436|    if (!anyPressed) {
437|        keyWasPressed = false;
438|        return;
439|    }
440|    
441|    if (keyWasPressed) return;
442|    keyWasPressed = true;
443|    
444|    auto keys = /* keysState replaced */;
445|
446|    // Handle sync modal
447|    if (syncModalActive) {
448|        if (syncState == SyncState::ERROR || syncState == SyncState::COMPLETE) {
449|            // Enter closes the modal after completion/error
450|            if (hal_input_wasPressed(KEY_ENTER) || hal_input_wasPressed(KEY_BACKSPACE)) {
451|                syncModalActive = false;
452|                syncState = SyncState::IDLE;
453|                scanCaptures();  // Rescan captures after sync
454|            }
455|        } else {
456|            // ESC cancels during sync
457|            if (hal_input_wasPressed(KEY_BACKSPACE)) {
458|                cancelSync();
459|            }
460|        }
461|        return;  // Block other inputs during sync
462|    }
463|
464|    // Handle nuke confirmation modal
465|    if (nukeConfirmActive) {
466|        if (hal_input_wasPressed('y') || hal_input_wasPressed('Y')) {
467|            nukeLoot();
468|            nukeConfirmActive = false;
469|            Display::clearBottomOverlay();
470|            scanCaptures();  // Refresh list (should be empty now)
471|        } else if (hal_input_wasPressed('n') || hal_input_wasPressed('N') ||
472|                   hal_input_wasPressed(KEY_BACKSPACE) || hal_input_wasPressed(KEY_ENTER)) {
473|            nukeConfirmActive = false;  // Cancel
474|            Display::clearBottomOverlay();
475|        }
476|        return;
477|    }
478|    
479|    // Handle detail view modal - Enter/backspace closes
480|    if (detailViewActive) {
481|        if (hal_input_wasPressed(KEY_ENTER) || hal_input_wasPressed(KEY_BACKSPACE)) {
482|            detailViewActive = false;
483|            return;
484|        }
485|        return;  // Block other inputs while detail view is open
486|    }
487|    
488|    // Navigation with ; (up) and . (down) — also rotates hints
489|    if (hal_input_wasPressed(KEY_UP)) {
490|        hintIndex = (hintIndex + 1) % HINT_COUNT;
491|        if (selectedIndex > 0) {
492|            selectedIndex--;
493|            if (selectedIndex < scrollOffset) {
494|                scrollOffset = selectedIndex;
495|            }
496|        }
497|    }
498|
499|    if (hal_input_wasPressed(KEY_DOWN)) {
500|        hintIndex = (hintIndex + 1) % HINT_COUNT;
501|