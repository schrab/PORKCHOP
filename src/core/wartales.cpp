#include "wartales.h"
#include "config.h"
#include "xp.h"
#include "sd_layout.h"
#include "../hal/hal_rtc.h"
#include <SD.h>
#include <stdio.h>

static File  sLogFile;
static bool  sOpen      = false;
static uint32_t sSessionStart = 0;
static uint16_t sEventCount   = 0;
static uint32_t sLastFlushMs  = 0;
static bool sSdReady = false;

static void ensureDir() {
    if (!sSdReady) return;
    const char* logsDir = SDLayout::logsDir();
    if (!SD.exists(logsDir)) {
        SD.mkdir(logsDir);
    }
}

static void writeLine(const char* line) {
    if (!sOpen || !sLogFile) return;
    uint32_t upSec = (millis() - sSessionStart) / 1000;
    char buf[96];
    int n = snprintf(buf, sizeof(buf) - 2, "[%3u:%02u] %s\n", upSec / 60, upSec % 60, line);
    if (n > 0 && n < (int)sizeof(buf)) {
        sLogFile.write((const uint8_t*)buf, n);
    }
    sEventCount++;
    sLastFlushMs = millis();
    if (sEventCount % 10 == 0) {
        sLogFile.flush();
    }
}

void Wartales::init() {
    sSessionStart = 0;
    sEventCount = 0;
    sOpen = false;
    sLastFlushMs = 0;
    sSdReady = SD.exists("/");
    if (!sSdReady) {
        Serial.println("[WARTALES] SD not available");
    }
}

void Wartales::sessionStart() {
    if (!sSdReady) {
        Serial.println("[WARTALES] SD not available");
        return;
    }
    sSessionStart = millis();
    sEventCount = 0;
    sLastFlushMs = millis();

    ensureDir();
    const char* logsDir = SDLayout::logsDir();

    // Find next available index
    uint32_t idx = 0;
    for (uint32_t i = 0; i < 999999; i++) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s/tale_%06lu.txt", logsDir, i);
        if (!SD.exists(buf)) { idx = i; break; }
    }

    char fname[64];
    snprintf(fname, sizeof(fname), "%s/tale_%06lu.txt", logsDir, idx);
    sLogFile = SD.open(fname, FILE_WRITE);
    if (!sLogFile) {
        Serial.printf("[WARTALES] Failed to create %s\r\n", fname);
        return;
    }
    sOpen = true;

    const char* hdr1 = "=== PORKCHOP — WAR TALE ===\n";
    sLogFile.write((const uint8_t*)hdr1, strlen(hdr1));
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Session start | LVL:%d XP:%lu",
             XP::getLevel(), (unsigned long)XP::getTotalXP());
    writeLine(hdr);
    Serial.printf("[WARTALES] %s\r\n", fname);
}

void Wartales::sessionEnd() {
    if (!sOpen) return;
    char footer[64];
    snprintf(footer, sizeof(footer), "Session end | events:%u", sEventCount);
    writeLine(footer);
    const char* footer_sep = "=================================\n";
    sLogFile.write((const uint8_t*)footer_sep, strlen(footer_sep));
    sLogFile.close();
    sOpen = false;
    Serial.printf("[WARTALES] session closed (%u events)\r\n", sEventCount);
}

void Wartales::update() {
    // Flush periodically even if not at 10 events
    uint32_t now = millis();
    if (sOpen && sLogFile && (now - sLastFlushMs >= 10000)) {
        sLogFile.flush();
        sLastFlushMs = now;
    }
}

void Wartales::logEvent(const char* event) {
    writeLine(event);
}

void Wartales::logCapture(const char* type, const char* ssid) {
    char buf[64];
    snprintf(buf, sizeof(buf), "CAPTURE %s: %.32s", type, ssid ? ssid : "?");
    writeLine(buf);
    if (sOpen && sLogFile) sLogFile.flush();
}

void Wartales::logDetection(const char* type, const char* detail) {
    char buf[72];
    snprintf(buf, sizeof(buf), "DETECT %s: %.40s", type, detail ? detail : "?");
    writeLine(buf);
    if (sOpen && sLogFile) sLogFile.flush();
}

void Wartales::logModeChange(const char* from, const char* to) {
    char buf[48];
    snprintf(buf, sizeof(buf), "MODE %s -> %s", from ? from : "?", to ? to : "?");
    writeLine(buf);
}
