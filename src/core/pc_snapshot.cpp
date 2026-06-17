#include "pc_snapshot.h"

#include <Arduino.h>

// RTC slow memory survives any reset, including TG1WDT_SYS_RST.
// `esp_restart()` does not touch RTC slow memory, so a watchdog that
// calls esp_restart() directly still leaves these values intact.
//
// checkpoint() writes these directly (no timer-based sampler) so the
// snapshot is always current even if the loop task starves the esp_timer
// task in the seconds before the WDT fires. The RTC write is a few
// hundred ns — negligible compared to the OINK work being measured.
static RTC_DATA_ATTR uint8_t  s_rtcPhase = PCSnapshot::PHASE_BOOT;
static RTC_DATA_ATTR uint32_t s_rtcMs    = 0;

static const char* phaseName(uint8_t p) {
    switch (p) {
        case PCSnapshot::PHASE_BOOT:               return "BOOT";
        case PCSnapshot::PHASE_PORKCHOP:           return "porkchop";
        case PCSnapshot::PHASE_OINK_DEQUEUE:       return "oink_dequeue";
        case PCSnapshot::PHASE_OINK_PROCESS_EAPOL: return "oink_process_eapol";
        case PCSnapshot::PHASE_OINK_AUTOSAVE:      return "oink_autosave";
        case PCSnapshot::PHASE_OINK_DEAUTH:        return "oink_deauth";
        default:                                   return "unknown";
    }
}

namespace PCSnapshot {

void checkpoint(Phase phase) {
    s_rtcPhase = static_cast<uint8_t>(phase);
    s_rtcMs    = millis();
}

void init() {
    // One-time self-test: write a sentinel to both RTC vars, read them
    // back, and clear. If this works, the RTC read/write path is verified
    // and any "no prior snapshot" on subsequent boots is the writes
    // truly not landing before the WDT fires. If the readback fails,
    // we'll see it in the log and know RTC is broken (e.g. a tool that
    // wipes RTC on reflash).
    s_rtcPhase = 0xAA;
    s_rtcMs    = 0xBBCCDDEE;
    Serial.printf("[PC-SNAPSHOT] self-test: wrote phase=0xAA ms=0xBBCCDDEE; readback phase=0x%02X ms=0x%08lX\r\n",
                  (unsigned)s_rtcPhase, (unsigned long)s_rtcMs);
    s_rtcPhase = PHASE_BOOT;
    s_rtcMs    = 0;
    Serial.println("[PC-SNAPSHOT] init: direct RTC writes (no sampler)");
}

void printStoredSnapshot() {
    // Always show raw values — do not early-return on zero, because that
    // hides the very case we need to debug (writes not landing in RTC).
    Serial.printf("[PC-SNAPSHOT] raw phase=%u ms=%u (name=%s)\r\n",
                  (unsigned)s_rtcPhase, (unsigned)s_rtcMs, phaseName(s_rtcPhase));

    if (s_rtcPhase == PHASE_BOOT && s_rtcMs == 0) {
        Serial.println("[PC-SNAPSHOT] no prior snapshot (clean boot or first run)");
        return;
    }

    Serial.printf("[PC-SNAPSHOT] PRIOR CRASH last=%s prev_boot_ms=%u (this boot is %u ms past it)\r\n",
                  phaseName(s_rtcPhase), (unsigned)s_rtcMs, (unsigned)millis());

    // Clear so a clean boot does not re-print the same stale data on
    // every restart. A second TG1WDT will overwrite with new data first.
    s_rtcPhase = PHASE_BOOT;
    s_rtcMs    = 0;
}

}  // namespace PCSnapshot
