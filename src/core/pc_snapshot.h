#pragma once

#include <cstdint>

// Phase checkpoint snapshot service for TG1WDT post-mortem.
//
// Captures which phase of the main loop was last entered before a watchdog
// reset. The most recent checkpoint is sampled at 1 Hz into RTC slow memory
// (survives any reset, including TG1WDT). On the next boot, the stored
// snapshot is printed to Serial before any other init so we know where
// the main loop was last healthy.
//
// This is intentionally coarse — phase IDs identify "which function we last
// entered", not an instruction address. A stack-walk based version was
// considered (see docs/tg1wdt_investigation.md) but rejected as fragile.
// Coarse data is strictly better than the current "PC inside panic_handler.c"
// we get from the ROM boot log.
namespace PCSnapshot {

// Phase IDs. Only the sites that matter for the OINK TG1WDT hunt are
// instrumented. PHASE_BOOT is the "no useful data" sentinel.
enum Phase : uint8_t {
    PHASE_BOOT             = 0,  // Sentinel: nothing meaningful recorded yet
    PHASE_PORKCHOP         = 1,  // porkchop.update() entered (mode dispatch)
    PHASE_OINK_DEQUEUE     = 2,  // OINK handshake dequeue loop (top of while)
    PHASE_OINK_PROCESS_EAPOL = 3, // OINK processing a dequeued EAPOL frame
    PHASE_OINK_AUTOSAVE    = 4,  // OINK autoSaveCheck (deferred SD write)
    PHASE_OINK_DEAUTH      = 5,  // OINK deauth TX burst
    PHASE_MAX              = 6
};

// Record that the main loop just entered `phase`. Writes directly to RTC
// slow memory (~hundreds of ns). Safe to call from any context. Does not
// block. The most recent call's phase is what gets printed on the next
// boot if a TG1WDT reset occurs.
void checkpoint(Phase phase);

// Hook for future setup. The first checkpoint() call primes the RTC values;
// no timer is required.
void init();

// Print the stored snapshot from the previous boot (if any) to Serial.
// Call from setup() as early as possible. Clears the stored snapshot
// after printing so a clean boot does not re-print the same data on
// every restart.
void printStoredSnapshot();

}  // namespace PCSnapshot
