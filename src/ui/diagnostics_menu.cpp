1|// Diagnostics Menu - System status snapshot
2|
3|#include "diagnostics_menu.h"
4|#include "../hal/hal_input.h"
#include "../hal/hal_display.h"
5|#include <SD.h>
6|#include <time.h>
7|#include <string.h>
8|#include "display.h"
9|#include "../core/config.h"
10|#include "../web/wpasec.h"
11|#include "../web/wigle.h"
12|#include "../core/sd_layout.h"
13|#include "../core/heap_health.h"
14|#include "../core/heap_policy.h"
15|#include "../core/wifi_utils.h"
16|#include <WiFi.h>
17|#include <esp_heap_caps.h>
18|#include <esp_wifi.h>
19|
20|// Static member initialization
21|bool DiagnosticsMenu::active = false;
22|bool DiagnosticsMenu::keyWasPressed = false;
23|uint16_t DiagnosticsMenu::cachedWpaCracked = 0;
24|uint16_t DiagnosticsMenu::cachedWigleUploaded = 0;
25|uint32_t DiagnosticsMenu::lastStatRefreshMs = 0;
26|uint32_t DiagnosticsMenu::statRefreshIntervalMs = 2000;  // tighter refresh interval
27|
28|void DiagnosticsMenu::show() {
29|    active = true;
30|    keyWasPressed = true;  // Ignore the Enter that brought us here
31|    lastStatRefreshMs = 0; // force immediate refresh
32|    HeapHealth::setKnuthEnabled(true);
33|}
34|
35|void DiagnosticsMenu::hide() {
36|    active = false;
37|    HeapHealth::setKnuthEnabled(false);
38|    // Release caches when leaving
39|    WPASec::freeCacheMemory();
40|    WiGLE::freeUploadedListMemory();
41|}
42|
43|void DiagnosticsMenu::update() {
44|    if (!active) return;
45|
46|    bool anyPressed = hal_input_anyHeld();
47|
48|    if (!anyPressed) {
49|        keyWasPressed = false;
50|        return;
51|    }
52|
53|    if (keyWasPressed) return;
54|    keyWasPressed = true;
55|
56|    auto keys = /* keysState replaced */;
57|
58|    // Enter/S - save snapshot
59|    if (hal_input_wasPressed(KEY_ENTER) || hal_input_wasPressed('s') || hal_input_wasPressed('S')) {
60|        saveSnapshot();
61|        Display::setTopBarMessage("DIAG SNAPSHOT SAVED", 3000);
62|        return;
63|    }
64|
65|    // R key - reset WiFi stack
66|    if (hal_input_wasPressed('r') || hal_input_wasPressed('R')) {
67|        resetWiFi();
68|        Display::setTopBarMessage("WIFI RESET", 3000);
69|        return;
70|    }
71|
72|    // H key - append quick heap log
73|    if (hal_input_wasPressed('h') || hal_input_wasPressed('H')) {
74|        logHeapSnapshot();
75|        Display::setTopBarMessage("HEAP LOGGED", 3000);
76|        return;
77|    }
78|
79|    // G key - free caches / pseudo GC
80|    if (hal_input_wasPressed('g') || hal_input_wasPressed('G')) {
81|        collectGarbage();
82|        Display::setTopBarMessage("CACHE CLEARED", 3000);
83|        return;
84|    }
85|
86|    // Periodically refresh stats (e.g., every 5 seconds)
87|    if (millis() - lastStatRefreshMs > statRefreshIntervalMs) {
88|        refreshStats();
89|    }
90|
91|    // Backspace - go back to previous menu
92|    if (hal_input_wasPressed(KEY_BACKSPACE)) {
93|        hide();
94|    }
95|}
96|
97|void DiagnosticsMenu::saveSnapshot() {
98|    // Create a diagnostic snapshot with system information
99|    if (!SD.exists("/")) {
100|        Display::notify(NoticeKind::WARNING, "NO SD CARD");
101|        return;
102|    }
103|
104|    // Generate filename with timestamp
105|    time_t now = time(nullptr);
106|    struct tm* timeinfo = localtime(&now);
107|    char filename[64];
108|    const char* diagDir = SDLayout::diagnosticsDir();
109|    const bool hasDiagDir = (strcmp(diagDir, "/") != 0);
110|    if (hasDiagDir && !SD.exists(diagDir)) {
111|        SD.mkdir(diagDir);
112|    }
113|    const char* sep = hasDiagDir ? "/" : "";
114|    snprintf(filename, sizeof(filename), "%s%sdiag_%04d%02d%02d_%02d%02d%02d.txt",
115|             diagDir, sep,
116|             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
117|             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
118|
119|    File file = SD.open(filename, FILE_WRITE);
120|    if (!file) {
121|        Display::notify(NoticeKind::WARNING, "SAVE FAILED");
122|        return;
123|    }
124|
125|    // Write system diagnostics
126|    file.printf("=== PORKCHOP DIAGNOSTICS SNAPSHOT ===\n");
127|    file.printf("Timestamp: %04d-%02d-%02d %02d:%02d:%02d\n",
128|                timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
129|                timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
130|    file.printf("\n");
131|
132|    // WiFi Status
133|    file.printf("WIFI STATUS:\n");
134|    file.printf("  Mode: %s\n", WiFi.getMode() == 0 ? "NULL" : WiFi.getMode() == 1 ? "STA" : WiFi.getMode() == 2 ? "AP" : "AP_STA");
135|    file.printf("  Status: %s\n", WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
136|    if (WiFi.status() == WL_CONNECTED) {
137|        file.printf("  SSID: %s\n", WiFi.SSID().c_str());
138|        file.printf("  IP: %s\n", WiFi.localIP().toString().c_str());
139|        file.printf("  MAC: %s\n", WiFi.macAddress().c_str());
140|    }
141|    file.printf("\n");
142|
143|    // Memory Status
144|    file.printf("MEMORY STATUS:\n");
145|    file.printf("  Free Heap: %u bytes\n", (unsigned int)ESP.getFreeHeap());
146|    file.printf("  Largest Block: %u bytes\n", (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
147|    file.printf("  Min Free Heap: %u bytes\n", (unsigned int)ESP.getMinFreeHeap());
148|    if (psramFound()) {
149|        file.printf("  PSRAM Size: %u bytes\n", (unsigned int)ESP.getPsramSize());
150|        file.printf("  PSRAM Free: %u bytes\n", (unsigned int)ESP.getFreePsram());
151|    }
152|    file.printf("\n");
153|
154|    // System Info
155|    file.printf("SYSTEM INFO:\n");
156|    file.printf("  SDK Version: %s\n", ESP.getSdkVersion());
157|    file.printf("  Chip Model: %s\n", ESP.getChipModel());
158|    file.printf("  Chip Cores: %d\n", ESP.getChipCores());
159|    file.printf("  CPU Freq: %d MHz\n", ESP.getCpuFreqMHz());
160|    file.printf("  Flash Size: %u MB\n", (unsigned int)(ESP.getFlashChipSize() / (1024 * 1024)));
161|    file.printf("\n");
162|
163|    // Battery Status
164|    file.printf("POWER STATUS:\n");
165|    file.printf("  Battery Voltage: %.2f V\n", hal_battery_read_mv() / 1000.0f);
166|    file.printf("  Battery Level: %d%%\n", hal_battery_read_percent());
167|    file.printf("  Is Charging: %s\n", hal_battery_isCharging() ? "YES" : "NO");
168|    file.printf("\n");
169|
170|    file.close();
171|}
172|
173|void DiagnosticsMenu::resetWiFi() {
174|    // Avoid driver teardown to prevent esp_wifi_init 257 on fragmented heap.
175|    WiFiUtils::hardReset();
176|}
177|
178|void DiagnosticsMenu::logHeapSnapshot() {
179|    if (!Config::isSDAvailable()) {
180|        Display::setTopBarMessage("NO SD CARD", 2000);
181|        return;
182|    }
183|    const char* heapPath = SDLayout::heapLogPath();
184|    const char* diagDir = SDLayout::diagnosticsDir();
185|    if (strcmp(diagDir, "/") != 0 && !SD.exists(diagDir)) {
186|        SD.mkdir(diagDir);
187|    }
188|    File f = SD.open(heapPath, FILE_APPEND);
189|    if (!f) {
190|        Display::setTopBarMessage("LOG FAILED", 2000);
191|        return;
192|    }
193|    time_t now = time(nullptr);
194|    struct tm* t = localtime(&now);
195|    f.printf("%04d-%02d-%02d %02d:%02d:%02d free=%u largest=%u min=%u min_largest=%u hmin_free=%u\n",
196|             t ? t->tm_year + 1900 : 0,
197|             t ? t->tm_mon + 1 : 0,
198|             t ? t->tm_mday : 0,
199|             t ? t->tm_hour : 0,
200|             t ? t->tm_min : 0,
201|             t ? t->tm_sec : 0,
202|             (unsigned int)ESP.getFreeHeap(),
203|             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
204|             (unsigned int)ESP.getMinFreeHeap(),
205|             (unsigned int)HeapHealth::getMinLargest(),
206|             (unsigned int)HeapHealth::getMinFree());
207|    f.close();
208|}
209|
210|void DiagnosticsMenu::collectGarbage() {
211|    // Free optional caches to claw back heap
212|    WPASec::freeCacheMemory();
213|    WiGLE::freeUploadedListMemory();
214|    delay(200);
215|    yield();
216|}
217|
218|void DiagnosticsMenu::refreshStats() {
219|    // Skip refresh if network ops are busy to avoid heap churn
220|    if (!WPASec::isBusy()) {
221|        cachedWpaCracked = WPASec::getCrackedCount();
222|    }
223|    if (!WiGLE::isBusy()) {
224|        cachedWigleUploaded = WiGLE::getUploadedCount();
225|    }
226|    WPASec::freeCacheMemory();
227|    WiGLE::freeUploadedListMemory();
228|    lastStatRefreshMs = millis();
229|}
230|
231|void DiagnosticsMenu::draw(DisplayCanvas& canvas) {
232|    if (!active) return;
233|
234|    canvas.fillSprite(COLOR_BG);
235|    canvas.setTextColor(COLOR_FG);
236|    canvas.setTextSize(1);
237|
238|    int y = 2;
239|    int lineH = 14;
240|
241|    // Heap
242|    canvas.drawString("HEAP:", 4, y);
243|    char heapBuf[16];
244|    snprintf(heapBuf, sizeof(heapBuf), "%u", (unsigned)ESP.getFreeHeap());
245|    canvas.drawString(heapBuf, 80, y);
246|    y += lineH;
247|    canvas.drawString("LARGEST:", 4, y);
248|    snprintf(heapBuf, sizeof(heapBuf), "%u", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
249|    canvas.drawString(heapBuf, 80, y);
250|    y += lineH;
251|    canvas.drawString("MIN FREE:", 4, y);
252|    snprintf(heapBuf, sizeof(heapBuf), "%u", (unsigned)ESP.getMinFreeHeap());
253|    canvas.drawString(heapBuf, 80, y);
254|    y += lineH;
255|    canvas.drawString("MIN LRG:", 4, y);
256|    snprintf(heapBuf, sizeof(heapBuf), "%u", (unsigned)HeapHealth::getMinLargest());
257|    canvas.drawString(heapBuf, 80, y);
258|    y += lineH;
259|
260|    // Pressure level
261|    {
262|        static const char* const pressureLabels[] = {"NORMAL", "CAUTION", "WARNING", "CRITICAL"};
263|        uint8_t pl = static_cast<uint8_t>(HeapHealth::getPressureLevel());
264|        canvas.drawString("PRESSURE:", 4, y);
265|        canvas.drawString(pl < 4 ? pressureLabels[pl] : "?", 80, y);
266|        y += lineH;
267|    }
268|
269|    // Knuth ratio (only meaningful when enabled via diagnostics)
270|    {
271|        char knBuf[16];
272|        float kr = HeapHealth::getKnuthRatio();
273|        snprintf(knBuf, sizeof(knBuf), "%.2f", kr);
274|        canvas.drawString("KNUTH:", 4, y);
275|        canvas.drawString(knBuf, 80, y);
276|        y += lineH;
277|    }
278|
279|    // Previous session watermarks
280|    {
281|        char prevBuf[16];
282|        uint32_t pmf = HeapHealth::getPrevMinFree();
283|        uint32_t pml = HeapHealth::getPrevMinLargest();
284|        if (pmf > 0 || pml > 0) {
285|            canvas.drawString("PREV MIN:", 4, y);
286|            snprintf(prevBuf, sizeof(prevBuf), "%u", pmf);
287|            canvas.drawString(prevBuf, 80, y);
288|            y += lineH;
289|            canvas.drawString("PREV LRG:", 4, y);
290|            snprintf(prevBuf, sizeof(prevBuf), "%u", pml);
291|            canvas.drawString(prevBuf, 80, y);
292|            y += lineH;
293|        }
294|    }
295|    y += 4;
296|
297|    // WiFi
298|    bool wifiUp = WiFi.status() == WL_CONNECTED;
299|    canvas.drawString("WIFI:", 4, y);
300|    canvas.drawString(wifiUp ? "CONNECTED" : "DISCONNECTED", 80, y);
301|    y += lineH;
302|    canvas.drawString("SSID:", 4, y);
303|    char ssidBuf[33];
304|    strncpy(ssidBuf, "-", sizeof(ssidBuf) - 1);
305|    ssidBuf[sizeof(ssidBuf) - 1] = '\0';
306|    if (wifiUp) {
307|        wifi_config_t conf;
308|        if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0]) {
309|            strncpy(ssidBuf, reinterpret_cast<const char*>(conf.sta.ssid), sizeof(ssidBuf) - 1);
310|            ssidBuf[sizeof(ssidBuf) - 1] = '\0';
311|        }
312|    }
313|    canvas.drawString(ssidBuf, 80, y);
314|    y += lineH;
315|    canvas.drawString("IP:", 4, y);
316|    char ipBuf[20];
317|    if (wifiUp) {
318|        IPAddress ip = WiFi.localIP();
319|        snprintf(ipBuf, sizeof(ipBuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
320|    } else {
321|        strncpy(ipBuf, "-", sizeof(ipBuf) - 1);
322|        ipBuf[sizeof(ipBuf) - 1] = '\0';
323|    }
324|    canvas.drawString(ipBuf, 80, y);
325|    y += lineH + 4;
326|
327|    // SD status / size
328|    canvas.drawString("SD:", 4, y);
329|    if (Config::isSDAvailable()) {
330|        uint64_t cardSize = SD.cardSize();
331|        uint64_t cardFree = SD.totalBytes() > SD.usedBytes() ? SD.totalBytes() - SD.usedBytes() : 0;
332|        uint32_t mb = (uint32_t)(cardSize / (1024ULL * 1024ULL));
333|        uint32_t freeMb = (uint32_t)(cardFree / (1024ULL * 1024ULL));
334|        char sdLine[24];
335|        snprintf(sdLine, sizeof(sdLine), "%u/%uMB", freeMb, mb);
336|        canvas.drawString(sdLine, 80, y);
337|    } else {
338|        canvas.drawString("MISSING", 80, y);
339|    }
340|    y += lineH + 4;
341|
342|    // Caches / uploads
343|    canvas.drawString("WPA-SEC:", 4, y);
344|    char cacheBuf[24];
345|    snprintf(cacheBuf, sizeof(cacheBuf), "%u CRACKED", (unsigned)cachedWpaCracked);
346|    canvas.drawString(cacheBuf, 80, y);
347|    y += lineH;
348|    canvas.drawString("WIGLE:", 4, y);
349|    snprintf(cacheBuf, sizeof(cacheBuf), "%u UPLOADED", (unsigned)cachedWigleUploaded);
350|    canvas.drawString(cacheBuf, 80, y);
351|    y += lineH + 6;
352|
353|    // Power
354|    canvas.drawString("BATT:", 4, y);
355|    char batt[32];
356|    snprintf(batt, sizeof(batt), "%d%% (%.2fV)", hal_battery_read_percent(), hal_battery_read_mv() / 1000.0f);
357|    canvas.drawString(batt, 80, y);
358|    y += lineH;
359|    canvas.drawString("CHARGING:", 4, y);
360|    canvas.drawString(hal_battery_isCharging() ? "YES" : "NO", 80, y);
361|    y += lineH + 6;
362|
363|    // Controls (compressed)
364|    canvas.drawString("[ENT]SAVE [R]WIFI", 4, y);
365|    y += lineH;
366|    canvas.drawString("[H]HEAP [G]GC [BKSPC]BACK", 4, y);
367|}
368|