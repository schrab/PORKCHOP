// SD Card Logger — ring-buffered, periodic flush
#pragma once

#include <Arduino.h>

class SDLog {
public:
    static void init();
    static void setEnabled(bool enabled);
    static bool isEnabled() { return logEnabled; }
    
    // Log functions - mirror Serial.printf behavior
    // Messages are buffered in RAM and flushed to SD periodically.
    static void log(const char* tag, const char* format, ...);
    static void logRaw(const char* message);
    
    // Flush ring buffer to SD (single open/write/close)
    static void flush();
    
    // Periodic flush — call from main loop.
    // Flushes if buffer >75% full or 30s since last flush.
    static void periodicFlush();
    
    // Close current log file (call on shutdown — flushes first)
    static void close();
    
private:
    static bool logEnabled;
    static bool initialized;
    static char currentLogFile[64];
    
    // 4KB ring buffer — absorbs transient SD failures, batches writes
    static char ringBuf[4096];
    static uint16_t ringHead;   // write position
    static uint16_t ringCount;  // bytes pending flush
    static uint32_t lastFlushMs;
    static uint32_t unflushedCount; // messages since last flush
    
    static void ensureLogFile();
    static void pushToRing(const char* line);
};

// Convenience macro - logs to both Serial and SD if enabled
// Always calls log() which does its own enabled check (more reliable across compilation units)
#define SDLOG(tag, fmt, ...) do { \
    Serial.printf("[%s] " fmt "\n", tag, ##__VA_ARGS__); \
    SDLog::log(tag, fmt, ##__VA_ARGS__); \
} while(0)
