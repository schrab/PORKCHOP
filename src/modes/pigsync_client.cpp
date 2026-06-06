1|/**
2| * PigSync ESP-NOW Client Implementation (Porkchop/POPS side)
3| * 
4| * SON OF A PIG - Reliable sync with Sirloin devices
5| */
6|
7|#include "pigsync_client.h"
8|#include "pigsync_protocol.h"
9|#include <esp_now.h>
10|#include <esp_wifi.h>
11|#include <WiFi.h>
12|#include <SD.h>
13|#include "../hal/hal_input.h"
14|#include <sys/time.h>  // Phase 3: settimeofday for RTC sync
15|#include "../core/config.h"
16|#include "../core/sdlog.h"
17|#include "../core/sd_layout.h"
18|#include "../core/wifi_utils.h"
19|#include "../core/heap_gates.h"
20|#include "../core/heap_policy.h"
21|#include "../core/network_recon.h"
22|#include "../piglet/mood.h"
23|#include "../ui/display.h"
24|#include "../modes/warhog.h"
25|#include "../modes/oink.h"
26|#include "../web/wpasec.h"
27|#include "../web/wigle.h"
28|
29|
30|#ifndef PIGSYNC_LOG_ENABLED
31|#define PIGSYNC_LOG_ENABLED 0
32|#endif
33|
34|#if PIGSYNC_LOG_ENABLED
35|#define PIGSYNC_LOGF(...) Serial.printf(__VA_ARGS__)
36|#define PIGSYNC_LOGLN(msg) Serial.println(msg)
37|#define PIGSYNC_LOG(msg) Serial.print(msg)
38|#else
39|#define PIGSYNC_LOGF(...) do {} while (0)
40|#define PIGSYNC_LOGLN(...) do {} while (0)
41|#define PIGSYNC_LOG(msg) do {} while (0)
42|#endif
43|
44|// ==[ STATIC MEMBER DEFINITIONS ]==
45|bool PigSyncMode::running = false;
46|bool PigSyncMode::initialized = false;
47|uint8_t PigSyncMode::selectedIndex = 0;
48|
49|std::vector<SirloinDevice> PigSyncMode::devices;
50|uint8_t PigSyncMode::connectedMac[6] = {0};
51|bool PigSyncMode::connected = false;
52|
53|uint16_t PigSyncMode::remotePMKIDCount = 0;
54|uint16_t PigSyncMode::remoteHSCount = 0;
55|uint16_t PigSyncMode::totalSynced = 0;
56|uint16_t PigSyncMode::syncedPMKIDs = 0;
57|uint16_t PigSyncMode::syncedHandshakes = 0;
58|
59|PigSyncMode::State PigSyncMode::state = PigSyncMode::State::IDLE;
60|
61|uint8_t PigSyncMode::currentType = 0;
62|uint16_t PigSyncMode::currentIndex = 0;
63|uint16_t PigSyncMode::totalChunks = 0;
64|uint16_t PigSyncMode::receivedChunks = 0;
65|
66|SyncProgress PigSyncMode::progress = {0};
67|uint8_t PigSyncMode::rxBuffer[2048] = {0};
68|uint16_t PigSyncMode::rxBufferLen = 0;
69|char PigSyncMode::lastError[64] = {0};
70|
71|uint8_t PigSyncMode::dialogueId = 0;
72|uint8_t PigSyncMode::dialoguePhase = 0;
73|uint32_t PigSyncMode::callStartTime = 0;
74|uint32_t PigSyncMode::phraseStartTime = 0;
75|char PigSyncMode::papaGoodbyeSelected[64] = {0};
76|
77|uint32_t PigSyncMode::lastDiscoveryTime = 0;
78|uint32_t PigSyncMode::discoveryStartTime = 0;
79|bool PigSyncMode::scanning = false;
80|uint32_t PigSyncMode::connectStartTime = 0;
81|uint32_t PigSyncMode::lastHelloTime = 0;
82|uint8_t PigSyncMode::helloRetryCount = 0;
83|uint32_t PigSyncMode::readyStartTime = 0;
84|uint8_t PigSyncMode::channelRetryCount = 0;
85|uint32_t PigSyncMode::syncCompleteTime = 0;
86|
87|uint16_t PigSyncMode::sessionId = 0;
88|uint8_t PigSyncMode::remoteMood = 128;
89|uint8_t PigSyncMode::lastBountyMatches = 0;
90|uint8_t PigSyncMode::dataChannel = PIGSYNC_DISCOVERY_CHANNEL;
91|
92|// Reliability tracking
93|static PigSyncReliability reliability;
94|
95|PigSyncMode::CaptureCallback PigSyncMode::onCaptureCb = nullptr;
96|PigSyncMode::SyncCompleteCallback PigSyncMode::onSyncCompleteCb = nullptr;
97|
98|// Peer info for connected Sirloin
99|static esp_now_peer_info_t sirloinPeer;
100|
101|// ==[ CONTROL TX RELIABILITY ]==
102|static const size_t CONTROL_TX_MAX = 160;
103|static const uint8_t CONTROL_QUEUE_MAX = 3;
104|struct ControlTxState {
105|    bool waiting;
106|    uint8_t type;
107|    uint8_t seq;
108|    uint32_t lastSend;
109|    uint8_t retries;
110|    size_t len;
111|    uint8_t mac[6];
112|    uint8_t buf[CONTROL_TX_MAX];
113|};
114|
115|static ControlTxState controlTx = {};
116|static ControlTxState controlQueue[CONTROL_QUEUE_MAX] = {};
117|static uint8_t controlQueueHead = 0;
118|static uint8_t controlQueueTail = 0;
119|static uint8_t controlQueueCount = 0;
120|static bool pendingStartSync = false;
121|static bool pendingNextCapture = false;
122|
123|static bool isControlCommand(uint8_t type) {
124|    switch (type) {
125|        case CMD_HELLO:
126|        case CMD_READY:
127|        case CMD_GET_COUNT:
128|        case CMD_MARK_SYNCED:
129|        case CMD_PURGE:
130|        case CMD_BOUNTIES:
131|        case CMD_TIME_SYNC:
132|            return true;
133|        default:
134|            return false;
135|    }
136|}
137|
138|static bool isControlResponse(uint8_t type) {
139|    switch (type) {
140|        case RSP_RING:
141|        case RSP_HELLO:
142|        case RSP_READY:
143|        case RSP_COUNT:
144|        case RSP_OK:
145|        case RSP_ERROR:
146|        case RSP_PURGED:
147|        case RSP_BOUNTIES_ACK:
148|        case RSP_TIME_SYNC:
149|        case RSP_DISCONNECT:
150|            return true;
151|        default:
152|            return false;
153|    }
154|}
155|
156|static bool isSessionBoundResponse(uint8_t type) {
157|    switch (type) {
158|        case RSP_READY:
159|        case RSP_OK:
160|        case RSP_ERROR:
161|        case RSP_DISCONNECT:
162|        case RSP_COUNT:
163|        case RSP_CHUNK:
164|        case RSP_COMPLETE:
165|        case RSP_PURGED:
166|        case RSP_BOUNTIES_ACK:
167|        case RSP_TIME_SYNC:
168|            return true;
169|        default:
170|            return false;
171|    }
172|}
173|
174|static void enqueueControl(const uint8_t* mac, const uint8_t* buf, size_t len, uint8_t type, uint8_t seq) {
175|    if (controlQueueCount >= CONTROL_QUEUE_MAX) {
176|        return;
177|    }
178|    ControlTxState& slot = controlQueue[controlQueueTail];
179|    memcpy(slot.buf, buf, len);
180|    slot.len = len;
181|    slot.type = type;
182|    slot.seq = seq;
183|    memcpy(slot.mac, mac, sizeof(slot.mac));
184|    slot.retries = 0;
185|    slot.waiting = true;
186|    slot.lastSend = 0;
187|    controlQueueTail = (controlQueueTail + 1) % CONTROL_QUEUE_MAX;
188|    controlQueueCount++;
189|}
190|
191|static bool dequeueControl(ControlTxState& out) {
192|    if (controlQueueCount == 0) {
193|        return false;
194|    }
195|    out = controlQueue[controlQueueHead];
196|    controlQueueHead = (controlQueueHead + 1) % CONTROL_QUEUE_MAX;
197|    controlQueueCount--;
198|    return true;
199|}
200|
201|static void resetControlQueue() {
202|    controlQueueHead = 0;
203|    controlQueueTail = 0;
204|    controlQueueCount = 0;
205|}
206|
207|static void sendControlPacket(const uint8_t* mac, const uint8_t* buf, size_t len, uint8_t type, uint8_t seq) {
208|    if (len > CONTROL_TX_MAX) {
209|        return;
210|    }
211|    if (!controlTx.waiting) {
212|        memcpy(controlTx.buf, buf, len);
213|        controlTx.len = len;
214|        controlTx.type = type;
215|        controlTx.seq = seq;
216|        memcpy(controlTx.mac, mac, sizeof(controlTx.mac));
217|        controlTx.retries = 0;
218|        controlTx.waiting = true;
219|        controlTx.lastSend = millis();
220|        esp_now_send(controlTx.mac, controlTx.buf, controlTx.len);
221|        return;
222|    }
223|    enqueueControl(mac, buf, len, type, seq);
224|}
225|
226|static void trySendQueuedControl() {
227|    if (controlTx.waiting) {
228|        return;
229|    }
230|    ControlTxState next = {};
231|    if (!dequeueControl(next)) {
232|        return;
233|    }
234|    controlTx = next;
235|    controlTx.lastSend = millis();
236|    esp_now_send(controlTx.mac, controlTx.buf, controlTx.len);
237|}
238|
239|static void clearControlTx() {
240|    controlTx.waiting = false;
241|    controlTx.len = 0;
242|    controlTx.type = 0;
243|    controlTx.seq = 0;
244|    controlTx.retries = 0;
245|    controlTx.lastSend = 0;
246|    trySendQueuedControl();
247|}
248|
249|static bool parseSirloinPMKID(const uint8_t* data, uint16_t len, CapturedPMKID& out) {
250|    if (!data || len < 65) {
251|        return false;
252|    }
253|
254|    memset(&out, 0, sizeof(out));
255|    memcpy(out.bssid, data, 6);
256|    memcpy(out.station, data + 6, 6);
257|
258|    uint8_t ssidLen = data[12];
259|    if (ssidLen > 32) ssidLen = 32;
260|    memcpy(out.ssid, data + 13, ssidLen);
261|    out.ssid[ssidLen] = '\0';
262|
263|    memcpy(out.pmkid, data + 45, 16);
264|    out.timestamp = millis();
265|    out.saved = false;
266|    out.saveAttempts = 0;
267|    return true;
268|}
269|
270|static bool parseSirloinHandshake(const uint8_t* data, uint16_t len, CapturedHandshake& out) {
271|    if (!data || len < 48) {
272|        return false;
273|    }
274|
275|    memset(&out, 0, sizeof(out));
276|    out.beaconData = nullptr;
277|    out.beaconLen = 0;
278|
279|    memcpy(out.bssid, data, 6);
280|    memcpy(out.station, data + 6, 6);
281|
282|    uint8_t ssidLen = data[12];
283|    if (ssidLen > 32) ssidLen = 32;
284|    memcpy(out.ssid, data + 13, ssidLen);
285|    out.ssid[ssidLen] = '\0';
286|
287|    size_t offset = 45;  // bssid(6) + station(6) + ssid_len(1) + ssid(32)
288|    if (offset + 3 > len) {
289|        return false;
290|    }
291|
292|    // Skip serialized mask (we recompute from parsed frames).
293|    offset += 1;
294|
295|    uint16_t beaconLen = data[offset] | (data[offset + 1] << 8);
296|    offset += 2;
297|
298|    if (beaconLen > 0) {
299|        if (beaconLen > 512 || offset + beaconLen > len) {
300|            return false;
301|        }
302|        out.beaconData = (uint8_t*)malloc(beaconLen);
303|        if (!out.beaconData) {
304|            return false;
305|        }
306|        memcpy(out.beaconData, data + offset, beaconLen);
307|        out.beaconLen = beaconLen;
308|        offset += beaconLen;
309|    }
310|
311|    out.capturedMask = 0;
312|    while (offset < len) {
313|        if (offset + 2 > len) break;
314|        uint16_t frameLen = data[offset] | (data[offset + 1] << 8);
315|        offset += 2;
316|        if (offset + frameLen > len) break;
317|        const uint8_t* frameData = data + offset;
318|        offset += frameLen;
319|
320|        if (offset + 2 > len) break;
321|        uint16_t fullLen = data[offset] | (data[offset + 1] << 8);
322|        offset += 2;
323|        if (offset + fullLen + 6 > len) break;  // msg+rss+ts
324|        const uint8_t* fullFrame = data + offset;
325|        offset += fullLen;
326|
327|        uint8_t msgNum = data[offset++];
328|        int8_t rssi = (int8_t)data[offset++];
329|        uint32_t ts = data[offset] |
330|                      (data[offset + 1] << 8) |
331|                      (data[offset + 2] << 16) |
332|                      (data[offset + 3] << 24);
333|        offset += 4;
334|
335|        if (msgNum < 1 || msgNum > 4) {
336|            continue;
337|        }
338|
339|        EAPOLFrame& frame = out.frames[msgNum - 1];
340|        uint16_t copyLen = frameLen;
341|        if (copyLen > sizeof(frame.data)) copyLen = sizeof(frame.data);
342|        memcpy(frame.data, frameData, copyLen);
343|        frame.len = copyLen;
344|
345|        uint16_t fullCopyLen = fullLen;
346|        if (fullCopyLen > sizeof(frame.fullFrame)) fullCopyLen = sizeof(frame.fullFrame);
347|        if (fullCopyLen > 0) {
348|            memcpy(frame.fullFrame, fullFrame, fullCopyLen);
349|        }
350|        frame.fullFrameLen = fullCopyLen;
351|        frame.messageNum = msgNum;
352|        frame.rssi = rssi;
353|        frame.timestamp = (ts < 1000000000) ? ts : millis();
354|
355|        out.capturedMask |= (1 << (msgNum - 1));
356|    }
357|
358|    out.firstSeen = millis();
359|    out.lastSeen = out.firstSeen;
360|    out.saved = false;
361|    out.saveAttempts = 0;
362|
363|    if (out.capturedMask == 0) {
364|        if (out.beaconData) {
365|            free(out.beaconData);
366|            out.beaconData = nullptr;
367|        }
368|        out.beaconLen = 0;
369|        return false;
370|    }
371|
372|    return true;
373|}
374|
375|static void removeIfExists(const char* path) {
376|    if (path && SD.exists(path)) {
377|        SD.remove(path);
378|    }
379|}
380|
381|static uint8_t getControlMaxRetries(uint8_t type) {
382|    if (type == CMD_HELLO) {
383|        uint16_t retries = PIGSYNC_HELLO_TIMEOUT / PIGSYNC_ACK_TIMEOUT;
384|        if (retries < 1) retries = 1;
385|        if (retries > 255) retries = 255;
386|        return (uint8_t)retries;
387|    }
388|    if (type == CMD_READY) {
389|        uint16_t retries = PIGSYNC_READY_TIMEOUT / PIGSYNC_ACK_TIMEOUT;
390|        if (retries < 1) retries = 1;
391|        if (retries > 255) retries = 255;
392|        return (uint8_t)retries;
393|    }
394|    return PIGSYNC_MAX_RETRIES;
395|}
396|
397|static void handleControlAck(const PigSyncHeader* hdr) {
398|    if (!controlTx.waiting) return;
399|    if (hdr->ack == controlTx.seq) {
400|        clearControlTx();
401|    }
402|}
403|
404|static volatile bool pendingControlAck = false;
405|static volatile uint8_t pendingControlAckSeq = 0;
406|
407|// Upgrade peer to encrypted after RSP_HELLO received
408|static void upgradePeerEncryption(const uint8_t* mac, uint8_t channel) {
409|    if (mac[0] == 0 && mac[1] == 0 && mac[2] == 0) return;
410|    
411|    // Remove and re-add with encryption on specified channel
412|    esp_now_del_peer(mac);
413|    
414|    memset(&sirloinPeer, 0, sizeof(sirloinPeer));
415|    memcpy(sirloinPeer.peer_addr, mac, 6);
416|    sirloinPeer.channel = channel;
417|    sirloinPeer.encrypt = true;
418|    memcpy(sirloinPeer.lmk, PIGSYNC_LMK, 16);
419|    
420|    esp_now_add_peer(&sirloinPeer);
421|    PIGSYNC_LOGF("[PIGSYNC-CLI-PEER] Upgraded to encrypted on ch%d\n", channel);
422|}
423|
424|// Pending data from callbacks (processed in update())
425|static volatile bool pendingRingReceived = false;
426|static volatile uint32_t pendingRingAt = 0;
427|static volatile bool pendingHelloReceived = false;
428|static volatile bool pendingHelloClearControl = false;
429|static volatile uint16_t pendingPMKIDCount = 0;
430|static volatile uint16_t pendingHSCount = 0;
431|static volatile uint8_t pendingDialogueId = 0;
432|static volatile uint8_t pendingMood = 128;
433|static volatile uint16_t pendingSessionId = 0;  // 16-bit session
434|static volatile uint8_t pendingDataChannel = PIGSYNC_DISCOVERY_CHANNEL;  // Data channel from RSP_HELLO
435|
436|static volatile bool pendingReadyReceived = false;  // RSP_READY from Sirloin
437|static volatile bool pendingReadyClearControl = false;
438|
439|static portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;
440|
441|// Name reveal notification for UI
442|static volatile bool pendingNameReveal = false;
443|static char pendingNameRevealName[16] = {0};
444|
445|static volatile bool pendingChunkReceived = false;
446|static const uint8_t PENDING_CHUNK_QUEUE_SIZE = 8;
447|struct PendingChunkSlot {
448|    bool used;
449|    uint16_t seq;
450|    uint16_t total;
451|    uint16_t len;
452|    uint8_t data[256];
453|};
454|static PendingChunkSlot pendingChunkQueue[PENDING_CHUNK_QUEUE_SIZE] = {};
455|static uint8_t pendingChunkCount = 0;
456|static void clearPendingChunkQueue() {
457|    taskENTER_CRITICAL(&pendingMux);
458|    for (uint8_t i = 0; i < PENDING_CHUNK_QUEUE_SIZE; i++) {
459|        pendingChunkQueue[i].used = false;
460|    }
461|    pendingChunkCount = 0;
462|    pendingChunkReceived = false;
463|    taskEXIT_CRITICAL(&pendingMux);
464|}
465|
466|static volatile bool pendingCompleteReceived = false;
467|static volatile uint16_t pendingTotalBytes = 0;
468|static volatile uint32_t pendingCRC = 0;
469|
470|static volatile bool pendingPurgedReceived = false;
471|static volatile uint16_t pendingPurgedCount = 0;
472|static volatile uint8_t pendingBountyMatches = 0;
473|
474|static volatile bool pendingErrorReceived = false;
475|static volatile uint8_t pendingErrorCode = 0;
476|
477|// Forward declarations for pending device updates
478|static volatile bool pendingBeaconReceived = false;
479|static uint8_t pendingBeaconMac[6] = {0};
480|static int8_t pendingBeaconRSSI = 0;
481|static uint16_t pendingBeaconPending = 0;
482|static uint8_t pendingBeaconFlags = 0;
483|
484|// Phase 3: Grunt beacon data
485|static volatile bool pendingGruntReceived = false;
486|static uint8_t pendingGruntMac[6] = {0};
487|static uint8_t pendingGruntFlags = 0;
488|static uint8_t pendingGruntCaptureCount = 0;
489|static uint8_t pendingGruntBattery = 0;
490|static uint8_t pendingGruntStorage = 0;
491|static uint32_t pendingGruntUnixTime = 0;
492|static uint16_t pendingGruntUptime = 0;
493|static char pendingGruntName[5] = {0};
494|
495|// Phase 3: Time sync response
496|static volatile bool pendingTimeSyncReceived = false;
497|static uint8_t pendingTimeSyncValid = 0;
498|static uint32_t pendingTimeSyncUnix = 0;
499|static uint32_t pendingTimeSyncRtt = 0;  // Round-trip time in ms
500|
501|