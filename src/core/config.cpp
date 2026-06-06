1|// src/core/config.cpp
2|// Configuration management implementation
3|
4|#include "config.h"
5|#include "sdlog.h"
6|#include "sd_layout.h"
7|// No M5Cardputer on ESP32-S3 Mini
8|#include <SD.h>
9|#include <SPIFFS.h>
10|#include <SPI.h>
11|#include <driver/gpio.h>
12|
13|// ---- Cardputer microSD wiring (explicit, per Cardputer v1.1 schematic) ----
14|// ESP32-S3FN8:
15|//   microSD Socket  CS   MOSI  CLK   MISO
16|//                  G12  G14   G40   G39
17|//
18|// (Your previous patch used ESP32 “classic” pins + CS=4, which breaks SD on Cardputer/StampS3.)
19|static constexpr int SD_CS_PIN   = 12;  // CS
20|static constexpr int SD_MOSI_PIN = 14;  // MOSI
21|static constexpr int SD_MISO_PIN = 39;  // MISO
22|static constexpr int SD_SCK_PIN  = 40;  // SCK/CLK
23|
24|// Dedicated SPI bus instance for SD.
25|// Cardputer microSD pinmap (from M5 docs): CS=12 MOSI=14 CLK=40 MISO=39.
26|// In practice, Arduino-ESP32/PlatformIO combos vary; using FSPI with explicit
27|// pins is the most reliable on Cardputer builds.
28|static SPIClass sdSPI(FSPI);
29|static bool sdSpiBegun = false;
30|
31|// Static member initialization
32|GPSConfig Config::gpsConfig;
33|MLConfig Config::mlConfig;
34|WiFiConfig Config::wifiConfig;
35|BLEConfig Config::bleConfig;
36|PersonalityConfig Config::personalityConfig;
37|bool Config::initialized = false;
38|static bool sdAvailable = false;
39|
40|// ---- Binary config blob (zero heap allocation) ----
41|static constexpr uint32_t CONFIG_MAGIC   = 0x504F524B;  // 'PORK'
42|static constexpr uint16_t CONFIG_VERSION = 1;
43|#define CONFIG_BIN_FILE "/porkchop.dat"
44|
45|static const char* configBinPathSD() {
46|    return SDLayout::usingNewLayout()
47|        ? "/m5porkchop/config/porkchop.dat"
48|        : "/porkchop.dat";
49|}
50|
51|struct __attribute__((packed)) ConfigBlob {
52|    uint32_t magic;
53|    uint16_t version;
54|    uint16_t blobSize;
55|
56|    // GPS
57|    uint8_t  gpsEnabled;
58|    uint8_t  gpsSource;
59|    uint8_t  gpsRxPin;
60|    uint8_t  gpsTxPin;
61|    uint32_t gpsBaudRate;
62|    uint16_t gpsUpdateInterval;
63|    uint16_t gpsSleepTimeMs;
64|    uint8_t  gpsPowerSave;
65|    int8_t   gpsTimezoneOffset;
66|
67|    // WiFi
68|    uint16_t channelHopInterval;
69|    uint16_t spectrumHopInterval;
70|    uint16_t lockTime;
71|    uint8_t  enableDeauth;
72|    uint8_t  randomizeMAC;
73|    int8_t   spectrumMinRssi;
74|    int8_t   attackMinRssi;
75|    uint8_t  spectrumTopN;
76|    uint16_t spectrumStaleMs;
77|    uint8_t  spectrumCollapseSsid;
78|    uint8_t  spectrumTiltEnabled;
79|    char     otaSSID[33];
80|    char     otaPassword[65];
81|    uint8_t  autoConnect;
82|    char     wpaSecKey[33];
83|    char     wigleApiName[65];
84|    char     wigleApiToken[65];
85|
86|    // BLE
87|    uint16_t burstInterval;
88|    uint16_t advDuration;
89|
90|    // ML (disabled but preserved for future)
91|    uint8_t  mlEnabled;
92|    uint8_t  mlCollectionMode;
93|    char     mlModelPath[64];
94|    float    mlConfidenceThreshold;
95|    float    mlRogueApThreshold;
96|    float    mlVulnScorerThreshold;
97|    uint8_t  mlAutoUpdate;
98|    char     mlUpdateUrl[128];
99|};
100|
101|static void populateBlob(ConfigBlob& b, const GPSConfig& gps, const WiFiConfig& wifi,
102|                          const BLEConfig& ble, const MLConfig& ml) {
103|    memset(&b, 0, sizeof(b));
104|    b.magic    = CONFIG_MAGIC;
105|    b.version  = CONFIG_VERSION;
106|    b.blobSize = sizeof(ConfigBlob);
107|
108|    b.gpsEnabled        = gps.enabled ? 1 : 0;
109|    b.gpsSource         = static_cast<uint8_t>(gps.source);
110|    b.gpsRxPin          = gps.rxPin;
111|    b.gpsTxPin          = gps.txPin;
112|    b.gpsBaudRate       = gps.baudRate;
113|    b.gpsUpdateInterval = gps.updateInterval;
114|    b.gpsSleepTimeMs    = gps.sleepTimeMs;
115|    b.gpsPowerSave      = gps.powerSave ? 1 : 0;
116|    b.gpsTimezoneOffset = gps.timezoneOffset;
117|
118|    b.channelHopInterval   = wifi.channelHopInterval;
119|    b.spectrumHopInterval  = wifi.spectrumHopInterval;
120|    b.lockTime             = wifi.lockTime;
121|    b.enableDeauth         = wifi.enableDeauth ? 1 : 0;
122|    b.randomizeMAC         = wifi.randomizeMAC ? 1 : 0;
123|    b.spectrumMinRssi      = wifi.spectrumMinRssi;
124|    b.attackMinRssi        = wifi.attackMinRssi;
125|    b.spectrumTopN         = wifi.spectrumTopN;
126|    b.spectrumStaleMs      = wifi.spectrumStaleMs;
127|    b.spectrumCollapseSsid = wifi.spectrumCollapseSsid ? 1 : 0;
128|    b.spectrumTiltEnabled  = wifi.spectrumTiltEnabled ? 1 : 0;
129|    strncpy(b.otaSSID,       wifi.otaSSID,       sizeof(b.otaSSID) - 1);
130|    strncpy(b.otaPassword,   wifi.otaPassword,   sizeof(b.otaPassword) - 1);
131|    b.autoConnect = wifi.autoConnect ? 1 : 0;
132|    strncpy(b.wpaSecKey,     wifi.wpaSecKey,     sizeof(b.wpaSecKey) - 1);
133|    strncpy(b.wigleApiName,  wifi.wigleApiName,  sizeof(b.wigleApiName) - 1);
134|    strncpy(b.wigleApiToken, wifi.wigleApiToken, sizeof(b.wigleApiToken) - 1);
135|
136|    b.burstInterval = ble.burstInterval;
137|    b.advDuration   = ble.advDuration;
138|
139|    b.mlEnabled              = ml.enabled ? 1 : 0;
140|    b.mlCollectionMode       = static_cast<uint8_t>(ml.collectionMode);
141|    strncpy(b.mlModelPath, ml.modelPath, sizeof(b.mlModelPath) - 1);
142|    b.mlConfidenceThreshold  = ml.confidenceThreshold;
143|    b.mlRogueApThreshold     = ml.rogueApThreshold;
144|    b.mlVulnScorerThreshold  = ml.vulnScorerThreshold;
145|    b.mlAutoUpdate           = ml.autoUpdate ? 1 : 0;
146|    strncpy(b.mlUpdateUrl, ml.updateUrl, sizeof(b.mlUpdateUrl) - 1);
147|}
148|
149|static bool writeBlobTo(fs::FS& fs, const char* path, const ConfigBlob& b) {
150|    File file = fs.open(path, FILE_WRITE);
151|    if (!file) {
152|        Serial.printf("[CONFIG] writeBlobTo: failed to open '%s'\n", path);
153|        return false;
154|    }
155|    size_t written = file.write((const uint8_t*)&b, sizeof(b));
156|    file.close();
157|    Serial.printf("[CONFIG] writeBlobTo: %u/%u bytes -> '%s'\n",
158|                  written, sizeof(b), path);
159|    return written == sizeof(b);
160|}
161|
162|static bool readBlobFrom(fs::FS& fs, const char* path, ConfigBlob& b) {
163|    File file = fs.open(path, FILE_READ);
164|    if (!file) return false;
165|
166|    size_t fileSize = file.size();
167|    if (fileSize < 8) { file.close(); return false; }  // too small for header
168|
169|    size_t readSize = (fileSize < sizeof(b)) ? fileSize : sizeof(b);
170|    memset(&b, 0, sizeof(b));
171|    size_t got = file.read((uint8_t*)&b, readSize);
172|    file.close();
173|
174|    if (got < 8 || b.magic != CONFIG_MAGIC) return false;
175|    Serial.printf("[CONFIG] readBlobFrom: '%s' v%u, %u bytes\n", path, b.version, got);
176|    return true;
177|}
178|
179|static void extractBlob(const ConfigBlob& b, GPSConfig& gps, WiFiConfig& wifi,
180|                         BLEConfig& ble, MLConfig& ml) {
181|    gps.enabled        = b.gpsEnabled != 0;
182|    gps.source         = static_cast<GPSSource>(b.gpsSource);
183|    gps.rxPin          = b.gpsRxPin;
184|    gps.txPin          = b.gpsTxPin;
185|    gps.baudRate       = b.gpsBaudRate;
186|    gps.updateInterval = b.gpsUpdateInterval;
187|    gps.sleepTimeMs    = b.gpsSleepTimeMs;
188|    gps.powerSave      = b.gpsPowerSave != 0;
189|    gps.timezoneOffset = b.gpsTimezoneOffset;
190|
191|    // Auto-set pins based on source (same as JSON loader)
192|    if (gps.source == GPSSource::CAP_LORA) {
193|        gps.rxPin = 15; gps.txPin = 13;
194|    } else if (gps.source == GPSSource::GROVE) {
195|        gps.rxPin = 1;  gps.txPin = 2;
196|    }
197|
198|    wifi.channelHopInterval   = b.channelHopInterval;
199|    wifi.spectrumHopInterval  = b.spectrumHopInterval;
200|    wifi.lockTime             = b.lockTime;
201|    wifi.enableDeauth         = b.enableDeauth != 0;
202|    wifi.randomizeMAC         = b.randomizeMAC != 0;
203|    wifi.spectrumMinRssi      = b.spectrumMinRssi;
204|    wifi.attackMinRssi        = b.attackMinRssi;
205|    wifi.spectrumTopN         = b.spectrumTopN;
206|    wifi.spectrumStaleMs      = b.spectrumStaleMs;
207|    wifi.spectrumCollapseSsid = b.spectrumCollapseSsid != 0;
208|    wifi.spectrumTiltEnabled  = b.spectrumTiltEnabled != 0;
209|    strncpy(wifi.otaSSID,       b.otaSSID,       sizeof(wifi.otaSSID) - 1);
210|    wifi.otaSSID[sizeof(wifi.otaSSID) - 1] = '\0';
211|    strncpy(wifi.otaPassword,   b.otaPassword,   sizeof(wifi.otaPassword) - 1);
212|    wifi.otaPassword[sizeof(wifi.otaPassword) - 1] = '\0';
213|    wifi.autoConnect = b.autoConnect != 0;
214|    strncpy(wifi.wpaSecKey,     b.wpaSecKey,     sizeof(wifi.wpaSecKey) - 1);
215|    wifi.wpaSecKey[sizeof(wifi.wpaSecKey) - 1] = '\0';
216|    strncpy(wifi.wigleApiName,  b.wigleApiName,  sizeof(wifi.wigleApiName) - 1);
217|    wifi.wigleApiName[sizeof(wifi.wigleApiName) - 1] = '\0';
218|    strncpy(wifi.wigleApiToken, b.wigleApiToken, sizeof(wifi.wigleApiToken) - 1);
219|    wifi.wigleApiToken[sizeof(wifi.wigleApiToken) - 1] = '\0';
220|
221|    ble.burstInterval = b.burstInterval;
222|    ble.advDuration   = b.advDuration;
223|
224|    ml.enabled              = b.mlEnabled != 0;
225|    ml.collectionMode       = static_cast<MLCollectionMode>(b.mlCollectionMode);
226|    strncpy(ml.modelPath, b.mlModelPath, sizeof(ml.modelPath) - 1);
227|    ml.modelPath[sizeof(ml.modelPath) - 1] = '\0';
228|    ml.confidenceThreshold  = b.mlConfidenceThreshold;
229|    ml.rogueApThreshold     = b.mlRogueApThreshold;
230|    ml.vulnScorerThreshold  = b.mlVulnScorerThreshold;
231|    ml.autoUpdate           = b.mlAutoUpdate != 0;
232|    strncpy(ml.updateUrl, b.mlUpdateUrl, sizeof(ml.updateUrl) - 1);
233|    ml.updateUrl[sizeof(ml.updateUrl) - 1] = '\0';
234|}
235|
236|static uint16_t clampU16(uint32_t value, uint16_t minVal, uint16_t maxVal) {
237|    if (value < minVal) return minVal;
238|    if (value > maxVal) return maxVal;
239|    return static_cast<uint16_t>(value);
240|}
241|
242|static int8_t clampI8(int value, int8_t minVal, int8_t maxVal) {
243|    if (value < minVal) return minVal;
244|    if (value > maxVal) return maxVal;
245|    return static_cast<int8_t>(value);
246|}
247|
248|static void sanitizeWiFiConfig(WiFiConfig& cfg) {
249|    cfg.channelHopInterval = clampU16(cfg.channelHopInterval, 50, 2000);
250|    cfg.spectrumHopInterval = clampU16(cfg.spectrumHopInterval, 50, 2000);
251|    cfg.spectrumMinRssi = clampI8(cfg.spectrumMinRssi, -95, -30);
252|    cfg.attackMinRssi = clampI8(cfg.attackMinRssi, -90, -50);
253|    if (cfg.spectrumTopN > 100) cfg.spectrumTopN = 100;
254|    cfg.spectrumStaleMs = clampU16(cfg.spectrumStaleMs, 1000, 20000);
255|}
256|
257|static void ensureSdSpiReady() {
258|    // Re-init SD SPI bus cleanly
259|    if (sdSpiBegun) {
260|        sdSPI.end();
261|        sdSpiBegun = false;
262|        delay(20);
263|    }
264|
265|    // Make sure CS is a sane GPIO output and deasserted before touching the bus.
266|    // This prevents random Select Failed errors on some cards.
267|    pinMode(SD_CS_PIN, OUTPUT);
268|    digitalWrite(SD_CS_PIN, HIGH);
269|
270|    // SCK, MISO, MOSI, SS/CS
271|    sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
272|    sdSpiBegun = true;
273|    delay(20);
274|}
275|
276|bool Config::init() {
277|    // Initialize SPIFFS first (always available)
278|    if (!SPIFFS.begin(false)) {
279|        Serial.println("[CONFIG] SPIFFS mount failed, attempting format...");
280|        if (!SPIFFS.begin(true)) {
281|            Serial.println("[CONFIG] SPIFFS format failed! Personality settings will not persist.");
282|        } else {
283|            Serial.println("[CONFIG] SPIFFS formatted and mounted OK");
284|        }
285|    }
286|
287|    // Allow buses to stabilize after HAL init
288|    delay(50);
289|
290|    // Ensure SD has a proper SPI bus configured
291|    ensureSdSpiReady();
292|
293|    // Retry with progressive SPI speeds for reliability
294|    sdAvailable = false;
295|    const int maxRetries = 6;
296|    const uint32_t speeds[] = {
297|        25000000, // 25 MHz
298|        20000000, // 20 MHz
299|        10000000, // 10 MHz
300|        8000000,  // 8 MHz
301|        4000000,  // 4 MHz
302|        1000000   // 1 MHz
303|    };
304|
305|    for (int attempt = 0; attempt < maxRetries && !sdAvailable; attempt++) {
306|        uint32_t speed = speeds[attempt];
307|        Serial.printf("[CONFIG] SD init attempt %d/%d at %luMHz\n",
308|                      attempt + 1, maxRetries, speed / 1000000);
309|
310|        if (attempt > 0) {
311|            SD.end();     // Clean up previous failed attempt
312|            delay(80);    // Allow bus to settle
313|            ensureSdSpiReady();
314|        }
315|
316|        // Use explicit CS + dedicated SPI + explicit speed
317|        if (SD.begin(SD_CS_PIN, sdSPI, speed)) {
318|            Serial.printf("[CONFIG] SD card mounted at %luMHz\n", speed / 1000000);
319|            sdAvailable = true;
320|        }
321|    }
322|
323|    if (!sdAvailable) {
324|        SDLayout::setUseNewLayout(false);
325|        Serial.println("[CONFIG] SD card init failed after retries, using SPIFFS");
326|    } else {
327|        SDLayout::migrateIfNeeded();
328|        SDLayout::ensureDirs();
329|        SDLog::log("CFG", "SD card mounted OK");
330|    }
331|
332|    // Load personality from SPIFFS (always available)
333|    if (!loadPersonality()) {
334|        Serial.println("[CONFIG] Creating default personality");
335|        createDefaultPersonality();
336|        savePersonalityToSPIFFS();
337|    }
338|
339|    // Load main config: SD primary, SPIFFS fallback
340|    Serial.printf("[CONFIG] Pre-load state: sdAvailable=%d, newLayout=%d\n",
341|                  sdAvailable, SDLayout::usingNewLayout());
342|    if (!load()) {
343|        Serial.println("[CONFIG] Creating default config");
344|        createDefaultConfig();
345|        save();
346|    }
347|
348|    // Try to load keys from files (auto-deletes after import)
349|    if (loadWpaSecKeyFromFile()) {
350|        Serial.println("[CONFIG] WPA-SEC key loaded from file");
351|    }
352|    if (loadWigleKeyFromFile()) {
353|        Serial.println("[CONFIG] WiGLE API keys loaded from file");
354|    }
355|
356|    // Merge creds from JSON porkchop.conf if present (handles the case where
357|    // binary config already exists but user dropped a new .conf with creds)
358|    if (importCredsFromJsonConf()) {
359|        Serial.println("[CONFIG] Credentials imported from porkchop.conf");
360|    }
361|
362|    initialized = true;
363|    return true;
364|}
365|
366|bool Config::isSDAvailable() {
367|    return sdAvailable;
368|}
369|
370|void Config::prepareSDBus() {
371|    ensureSdSpiReady();
372|}
373|
374|SPIClass& Config::sdSpi() {
375|    return sdSPI;
376|}
377|
378|int Config::sdCsPin() {
379|    return SD_CS_PIN;
380|}
381|
382|void Config::prepareCapLoraGpio() {
383|    // GPIO 13 is ESP32-S3 default FSPIQ (MISO) via IOMUX. Even though SD remaps
384|    // FSPI MISO to G39, the default IOMUX linkage on G13 can disrupt the FSPI
385|    // peripheral when Serial2 reconfigures G13 as UART TX output.
386|    // gpio_reset_pin() clears IOMUX function, disconnects peripheral signals,
387|    // and returns the pin to plain GPIO mode.
388|    gpio_reset_pin(static_cast<gpio_num_t>(CapLoraPins::GPS_TX));   // G13
389|
390|    // Reset SX1262 LoRa chip to known state. The CapLoRa868 LoRa SPI shares
391|    // MOSI(G14)/MISO(G39)/SCK(G40) with SD card. After reset the SX1262
392|    // enters STANDBY_RC with all IOs high-impedance, preventing bus contention.
393|    pinMode(CapLoraPins::LORA_RESET, OUTPUT);
394|    digitalWrite(CapLoraPins::LORA_RESET, LOW);   // Assert NRESET (active low)
395|    delay(10);                                      // SX1262 datasheet: >100us
396|    digitalWrite(CapLoraPins::LORA_RESET, HIGH);   // Release reset
397|    delay(10);                                      // Wait for standby entry
398|
399|    // Deassert LoRa chip select (HIGH = not selected, MISO tri-stated)
400|    pinMode(CapLoraPins::LORA_CS, OUTPUT);
401|    digitalWrite(CapLoraPins::LORA_CS, HIGH);
402|
403|    // Configure control pins as inputs (don't drive)
404|    pinMode(CapLoraPins::LORA_BUSY, INPUT);
405|    pinMode(CapLoraPins::LORA_DIO1, INPUT);
406|
407|    Serial.println("[CONFIG] CapLoRa868: SX1262 reset, CS deasserted, G13 IOMUX cleared");
408|}
409|
410|bool Config::reinitSD() {
411|    // Quick check: SD still accessible? Skip destructive reinit if so.
412|    if (sdAvailable && SD.exists("/")) {
413|        Serial.println("[CONFIG] SD still accessible after GPS init, skipping reinit");
414|        return true;
415|    }
416|
417|    Serial.println("[CONFIG] SD access lost, attempting re-initialization...");
418|
419|    // Save current state to restore on failure
420|    bool wasSdAvailable = sdAvailable;
421|    bool wasNewLayout = SDLayout::usingNewLayout();
422|
423|    // Clean up any existing SD state
424|    SD.end();
425|    delay(80);
426|
427|    // Re-init SD SPI bus explicitly
428|    ensureSdSpiReady();
429|
430|    // Retry with progressive SPI speeds
431|    sdAvailable = false;
432|    const int maxRetries = 6;
433|    const uint32_t speeds[] = {
434|        25000000,
435|        20000000,
436|        10000000,
437|        8000000,
438|        4000000,
439|        1000000
440|    };
441|
442|    for (int attempt = 0; attempt < maxRetries && !sdAvailable; attempt++) {
443|        uint32_t speed = speeds[attempt];
444|        Serial.printf("[CONFIG] SD reinit attempt %d/%d at %luMHz\n",
445|                      attempt + 1, maxRetries, speed / 1000000);
446|
447|        if (attempt > 0) {
448|            SD.end();
449|            delay(80);
450|            ensureSdSpiReady();
451|        }
452|
453|        if (SD.begin(SD_CS_PIN, sdSPI, speed)) {
454|            Serial.printf("[CONFIG] SD card mounted at %luMHz\n", speed / 1000000);
455|            sdAvailable = true;
456|        }
457|    }
458|
459|    if (sdAvailable) {
460|        // Success: verify layout by checking marker directly (no full migration)
461|        if (SD.exists(SDLayout::migrationMarkerPath())) {
462|            SDLayout::setUseNewLayout(true);
463|        } else if (wasNewLayout) {
464|            // Marker unreadable but we were using new layout — keep it
465|            SDLayout::setUseNewLayout(true);
466|        }
467|        SDLayout::ensureDirs();
468|        SDLog::log("CFG", "SD card re-initialized OK");
469|    } else {
470|        // FAIL: Restore previous state — don't corrupt flags
471|        sdAvailable = wasSdAvailable;
472|        SDLayout::setUseNewLayout(wasNewLayout);
473|        Serial.println("[CONFIG] SD reinit failed, keeping previous SD state");
474|    }
475|
476|    return sdAvailable;
477|}
478|
479|bool Config::loadFrom(fs::FS& fs, const char* path) {
480|    File file = fs.open(path, FILE_READ);
481|    if (!file) return false;
482|
483|    size_t fileSize = file.size();
484|    Serial.printf("[CONFIG] loadFrom(): '%s' size=%u bytes\n", path, fileSize);
485|    if (fileSize == 0) { file.close(); return false; }
486|
487|    JsonDocument doc;
488|    DeserializationError err = deserializeJson(doc, file);
489|    file.close();
490|
491|    if (err) {
492|        Serial.printf("[CONFIG] loadFrom(): JSON error: %s ('%s')\n", err.c_str(), path);
493|        return false;
494|    }
495|
496|    // Populate config from parsed JSON — shared with load()
497|    return applyJson(doc);
498|}
499|
500|bool Config::applyJson(const JsonDocument& doc) {
501|