# src/core — Core State Machine & Services

## Purpose

Central state machine, persistent configuration, XP/leveling, background WiFi scanning, session diary, MAC randomization, heap management, SD I/O, OUI lookup, and stress testing.

## Ownership

- `porkchop.cpp` — main state machine (mode dispatch, event queue, boot mode auto-entry)
- `config.cpp` — persistent config (JSON on SD, NVS fallback)
- `xp.cpp` — RPG XP / leveling (NVS primary, SD backup)
- `network_recon.cpp` — background WiFi promiscuous scanning
- `wartales.cpp` — session diary (open file handle, flush every 10 events / 10s)
- `ghost.cpp` — MAC randomization (Ghost Mode)
- `sd_layout.cpp` — SD card directory structure + migration
- `sdlog.cpp` — SD event logging (4KB ring buffer, periodic flush every 30s or 75% full)
- `sd_format.cpp` — SD card format utilities
- `heap_health.cpp` — heap diagnostics (NVS watermarks, SD fallback migration)
- `heap_gates.cpp` — heap gate checks before allocations
- `heap_policy.h` — heap allocation policy constants (thresholds, watermarks)
- `wifi_utils.cpp` — MAC address utilities, SSID helpers, WiFi config
- `oui.cpp` — OUI (MAC vendor) lookup
- `challenges.cpp` — session challenge definitions and tracking
- `stress_test.cpp` — heap stress testing utilities
- `wsl_bypasser.cpp` — deauth frame construction (WSL bypass)
- `pc_snapshot.cpp` — phase checkpoint writer for TG1WDT post-mortem (RTC-backed)
- `logging.h` — compile-time serial logging macros (header only)

## Local Contracts

### ConfigBlob struct
- Packed, `CONFIG_VERSION=1`. New fields append at end.
- Old blobs zero-initialize new fields via `memset`.
- JSON path: `/porkchop.conf` (new layout) or legacy root.

### NetworkRecon threading
- `enterCritical()` / `exitCritical()` protect the shared `networks[]` vector.
  Safe to call from Core 1 (main thread) but MUST NOT be called from
  Core 0 (promiscuous callback context) — cross-core spinlock contention blocks
  Core 0 with interrupts disabled → TG1WDT.
- Mode callbacks are **PURE ENQUEUERS**: copy raw frame data to mode-owned ring buffers,
  no vector iteration. Core 1 dequeue handlers do all vector lookups and writes.
- `oinkQueueMux` ONLY protects the OINK queue (`pendingHsPool[]`), not capture vectors.
- `setPacketCallback()` supports only **one** callback at a time. SnOUT registers on start, clears on stop. Do not run alongside other callback-using modes.
- `pause()` for ESP-NOW modes (PIGSYNC). `stop()` for BLE modes (PIGGYBLUES).

### WarTales flush cadence
- Open file handle per session. Flush every 10 events or 10s.
- `logEvent()`, `logCapture()`, `logDetection()` for mode starts and key actions.

### XP persistence
- NVS primary (`porkxp` namespace). SD backup throttled to 120s intervals via `periodicBackup()`.
- `restoreFromSD()` on boot if NVS is fresh. Backup on IDLE transition.
- `processPendingSave()` called from mode loops (deferred save).

### SD layout
- New root: `/porkchop`. Legacy root: `/` (migration via `migrateIfNeeded()`).
- `SDLayout::` namespace resolves paths for both layouts.
- `ensureDirs()` creates all required directories.

### PCSnapshot
- `checkpoint(Phase)` writes the phase ID + `millis()` directly to RTC slow
  memory. Survives any reset including TG1WDT_SYS_RST.
- No timer-based sampler — a 1 Hz `esp_timer` snapshot was tried first and
  the sampler was starved in the seconds before the WDT fired, never
  writing RTC and defeating the purpose. Direct writes are immune to that
  and cost ~hundreds of ns.
- `printStoredSnapshot()` must be called from `setup()` before any other init
  that could interleave output. Clears the stored snapshot after printing.
- Instrument only the sites that answer a specific debugging question.
  Adding markers to "everything" dilutes the signal: the OINK hunt uses
  exactly 5 sites (`PORKCHOP` + 4 OINK sub-phases). See
  `docs/tg1wdt_investigation.md` for rationale.

## Child DOX Index

| Child | Scope |
|---|---|
| `core/porkchop.*` | State machine, mode dispatch, event system |
| `core/config.*` | Persistent config management |
| `core/xp.*` | XP / leveling / achievements |
| `core/network_recon.*` | Background WiFi scanning service |
| `core/wartales.*` | Session diary logging |
| `core/ghost.*` | MAC randomization |
| `core/sd_layout.*` | SD card directory structure |
| `core/sd_format.*` | SD card format utilities |
| `core/sdlog.*` | SD event logging |
| `core/heap_*` | Heap management & diagnostics |
| `core/heap_policy.*` | Heap allocation policy constants |
| `core/wifi_utils.*` | MAC/SSID utilities |
| `core/oui.*` | OUI vendor lookup |
| `core/challenges.*` | Session challenges |
| `core/stress_test.*` | Heap stress testing |
| `core/wsl_bypasser.*` | Deauth frame construction |
| `core/pc_snapshot.*` | Phase checkpoint writer (TG1WDT post-mortem) |
| `core/logging.h` | Logging macros (header only) |
