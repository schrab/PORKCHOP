// SD Card Logger implementation — ring-buffered, periodic flush

#include "sdlog.h"
#include "config.h"
#include "sd_layout.h"
#include <SD.h>
#include <stdarg.h>

bool SDLog::logEnabled = false;
bool SDLog::initialized = false;
char SDLog::currentLogFile[64] = {0};

// 4KB ring buffer
char SDLog::ringBuf[4096];
uint16_t SDLog::ringHead = 0;
uint16_t SDLog::ringCount = 0;
uint32_t SDLog::lastFlushMs = 0;
uint32_t SDLog::unflushedCount = 0;

static const uint32_t FLUSH_INTERVAL_MS = 30000;
static const uint16_t RING_SIZE = 4096;
static const uint16_t RING_FLUSH_THRESHOLD = RING_SIZE * 3 / 4; // 75%

void SDLog::init() {
    initialized = true;
    ringHead = 0;
    ringCount = 0;
    lastFlushMs = millis();
    unflushedCount = 0;
}

void SDLog::setEnabled(bool enabled) {
    Serial.printf("[SDLOG] setEnabled(%s), SD available: %s\r\n",
                  enabled ? "true" : "false",
                  Config::isSDAvailable() ? "true" : "false");

    logEnabled = enabled && Config::isSDAvailable();

    if (logEnabled && currentLogFile[0] == '\0') {
        ensureLogFile();
    }

    if (logEnabled) {
        Serial.printf("[SDLOG] Logging now ENABLED to: %s\r\n", currentLogFile);
        log("SDLOG", "SD logging enabled");
    } else {
        Serial.printf("[SDLOG] Logging DISABLED\r\n");
    }
}

void SDLog::ensureLogFile() {
    if (currentLogFile[0] != '\0') return;
    if (!Config::isSDAvailable()) return;

    const char* logsDir = SDLayout::logsDir();
    if (!SD.exists(logsDir)) {
        SD.mkdir(logsDir);
    }

    snprintf(currentLogFile, sizeof(currentLogFile), "%s/porkchop.log", logsDir);

    File f = SD.open(currentLogFile, FILE_WRITE);
    if (f) {
        f.println("=== PORKCHOP LOG ===");
        f.printf("Started at millis: %lu\n", millis());
        f.println("====================");
        f.close();
        Serial.printf("[SDLOG] Log file: %s\r\n", currentLogFile);
    } else {
        Serial.printf("[SDLOG] Failed to create: %s\r\n", currentLogFile);
        currentLogFile[0] = '\0';
    }
}

void SDLog::pushToRing(const char* line) {
    uint16_t len = strlen(line);
    if (len == 0) return;

    // If a single line exceeds remaining ring space, flush first
    if (len >= RING_SIZE - ringCount) {
        flush();
    }
    // If still too large after flush, skip (extremely long lines)
    if (len >= RING_SIZE) return;

    // Wrap-around copy
    uint16_t space = RING_SIZE - ringHead;
    if (len <= space) {
        memcpy(ringBuf + ringHead, line, len);
    } else {
        memcpy(ringBuf + ringHead, line, space);
        memcpy(ringBuf, line + space, len - space);
    }
    ringHead = (ringHead + len) % RING_SIZE;
    ringCount += len;
    unflushedCount++;
}

void SDLog::log(const char* tag, const char* format, ...) {
    if (!logEnabled) return;
    if (currentLogFile[0] == '\0') {
        ensureLogFile();
        if (currentLogFile[0] == '\0') return;
    }
    
    char line[256];
    // Format timestamp + tag + message into one line
    va_list args;
    va_start(args, format);
    int prefixLen = snprintf(line, sizeof(line), "[%lu][%s] ", millis(), tag);
    vsnprintf(line + prefixLen, sizeof(line) - prefixLen, format, args);
    va_end(args);
    strcat(line, "\n");
    
    Serial.printf("[SDLOG->RING] %s", line);
    pushToRing(line);
}

void SDLog::logRaw(const char* message) {
    if (!logEnabled) return;
    if (currentLogFile[0] == '\0') {
        ensureLogFile();
        if (currentLogFile[0] == '\0') return;
    }

    char line[256];
    snprintf(line, sizeof(line), "%s\n", message);
    pushToRing(line);
}

void SDLog::flush() {
    if (ringCount == 0) return;
    if (!Config::isSDAvailable()) return;
    if (currentLogFile[0] == '\0') return;

    File f = SD.open(currentLogFile, FILE_APPEND);
    if (!f) {
        // SD busy or unavailable — drop buffered data, next flush retries
        Serial.printf("[SDLOG] flush: failed to open, dropping %u bytes\r\n", ringCount);
        ringHead = 0;
        ringCount = 0;
        unflushedCount = 0;
        return;
    }

    // Write from tail to head (may wrap around)
    uint16_t tail = (ringHead + RING_SIZE - ringCount) % RING_SIZE;
    uint16_t remaining = ringCount;

    while (remaining > 0) {
        uint16_t chunk = (tail + remaining <= RING_SIZE) ? remaining : RING_SIZE - tail;
        f.write((const uint8_t*)(ringBuf + tail), chunk);
        tail = (tail + chunk) % RING_SIZE;
        remaining -= chunk;
    }
    f.close();

    lastFlushMs = millis();
    ringHead = 0;
    ringCount = 0;
    unflushedCount = 0;
}

void SDLog::periodicFlush() {
    if (!logEnabled) return;
    if (ringCount == 0) return;

    uint32_t now = millis();
    bool thresholdReached = ringCount >= RING_FLUSH_THRESHOLD;
    bool intervalElapsed = (now - lastFlushMs) >= FLUSH_INTERVAL_MS;

    if (thresholdReached || intervalElapsed) {
        flush();
    }
}

void SDLog::close() {
    if (logEnabled && currentLogFile[0] != '\0') {
        log("SDLOG", "Log closed");
        flush();  // Drain remaining data before close
    }
    currentLogFile[0] = '\0';
}
