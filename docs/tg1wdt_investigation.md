# OINK TG1WDT Investigation — Status Report

**Date:** 2026-06-16
**Branch:** `esp32-s3-mini-port`
**Firmware:** custom ESP32-S3 Mini port of PORKCHOP (Arduino, PlatformIO)

## Problem statement

OINK mode on the ESP32-S3 Mini custom board triggers a Task Watchdog
timeout (TG1WDT_SYS_RST) during operation. The crash is inconsistent:
sometimes after 10 TXs, sometimes after several minutes, sometimes during
background scanning with no deauths in flight. The device reboots and
shows `Saved PC:0x420759bb` (and other nearby values) in the ROM boot
log, all of which resolve to **`panic_handler.c` in the ESP-IDF
framework** — the actual crash site is lost.

## What we know (with evidence)

### 1. Crash is TG1WDT, not a panic

`rst:0x8 (TG1WDT_SYS_RST)` in the boot log. The ESP-IDF coredump
subsystem only writes a coredump on real panics (exception, assert).
Task Watchdog triggers `esp_restart()` directly without going through
the panic handler, so the coredump partition is never written. The
coredump we read from flash (SHA `17cef689…`) is stale — from a
different crash that did go through the panic handler — and does not
match the current firmware's SHA (`8e79eb7b…`).

**Implication:** we cannot use ESP-IDF coredump to debug TG1WDT resets.

### 2. Heap is healthy at crash time

Across many crash logs, the `[OINK-DIAG] t=…` line printed ~2s before
each crash shows:
- `free=66000…75000` bytes
- `largest=42000…65000` bytes
- `auto=1..4`, `hs=0..2`, `deauthTxOk=0..226`, `deauthTxErr=0..522`

Heap exhaustion is **not** the cause.

### 3. ERR 257 was a real issue, mostly fixed

`esp_wifi_80211_tx()` returned `ESP_ERR_NO_MEM` on most deauths.
Root cause: Arduino's `WiFiGeneric.cpp` hardcodes
`dynamic_tx_buf_num=32` at `esp_wifi_init()`, ignoring sdkconfig.

Fix applied (commit `2fd5c57`):
- `scripts/patch_wifi_buffers.py` patches the framework file to set
  `dynamic_tx_buf_num=128` (idempotent, auto-applied on every build).
- `WiFi.disconnect(false, false)` in `NetworkRecon::start()` skips
  `esp_wifi_set_config()` which can reset the pool.
- Exponential backoff circuit breaker in `sendDeauthFrame`:
  50ms → 100ms → 200ms → 400ms cap, resets on OK.

Verified: first 30 TXs all OK, ERR ratio improves from 5:1 to ~0.6:1
once backoff engages. The buffer patch only bumps dynamic TX (not
RX/cache/static) to keep internal RAM low enough that the 90KB main
canvas sprite still allocates.

### 4. Dequeue loop CPU starvation — partial fix

`OinkMode::update()` had a `while (true)` dequeue loop processing up to
4 queued EAPOL frames per loop tick with no `yield()` between iterations.
Each iteration held `oinkQueueMux` + `NetworkRecon::vectorMux`
(cross-core spinlock) for hundreds of microseconds.

Fix applied (commit `2fd5c57`, refined in `225b40a`):
- Cap at 2 frames/tick
- `yield()` between iterations
- Same pattern for PMKID dequeue
- `yield()` after SSID-lookup spinlock in dequeue

### 5. `autoSaveCheck()` was called inside the dequeue loop

When a handshake completed inside `findOrCreateHandshakeSafe`, the
loop called `autoSaveCheck()` which did 100-500ms of blocking SD I/O
(PCAP + 22000 format). Combined with the dequeue's spinlock work, this
starved the Core 0 WiFi task watchdog.

Fix applied (commit `225b40a`): removed `autoSaveCheck()` from the
dequeue, set `pendingAutoSave=true` instead. The save runs at the top
of the next `update()` tick, outside the dequeue/spinlock work.

### 6. `yield()` added in LOCKING and writePCAPPacket

Both from commit `225b40a`. Speculative — should help but unverified.

## What we don't know

### 1. Actual crash site

Every crash PC (`0x4207593b`, `0x42075987`, `0x4207599b`, `0x420759ab`,
`0x420759bb`) resolves to `panic_handler.c` in ESP-IDF. This is the
PC **inside the watchdog reset path**, not the crash site. The
actual PC at the moment of the WDT fire is lost.

We can add `ESP_EARLY_LOG` to a watchdog timeout handler (not yet
done) to capture the PC before restart. **Without that, any fix is
a guess.**

### 2. Which Core 1 task is starving

The TG1WDT fires on the Arduino main task (loop). Possible culprits:
- OINK `update()` taking >30s without yielding
- `Porkchop::update()` in `core/porkchop.cpp` — heavy chain of
  `yield()`s but each only briefly
- `Display::update()` — sprite flush can take milliseconds
- The `WiFi` event handler on Core 0 — not under our control

We have not measured which path is slow. The 2-second diag interval
hides any task that runs for less than 2s.

### 3. Why ERR 257 persists at ~40% failure rate

After the patch, ~40% of deauths still fail with ESP_ERR_NO_MEM. Heap
is fine. The WiFi driver holds TX descriptors waiting for ACKs that
never come (deauth is one-way; no ACK). The pool drains and never
fully refills. We could try 256-entry pool but that costs ~31KB
internal RAM and may not help.

### 4. What the user is actually seeing in the field

The user's testing is on a real network. They confirm:
- SPECTRUM mode works (no crashes in 30+ min tests, captures handshakes)
- OINK mode crashes — sometimes immediately, sometimes after
  handshakes, sometimes after manual AP reconnects
- "Saved PC" varies, "auto" state varies, "hs" varies — no single
  reproducible pattern

## Fixes applied (chronological)

| Commit | Change | Verified? |
|---|---|---|
| `2fd5c57` | WiFi dynamic TX 32→128 via framework patch | Yes — deauths reach air, ERR ratio improves |
| `2fd5c57` | `WiFi.disconnect(false, false)` in NetworkRecon | Yes — deauths work after OINK entry |
| `2fd5c57` | Exponential backoff in `sendDeauthFrame` | Yes — ERR stops runaway, OK rate climbs |
| `2fd5c57` | Dequeue loop cap at 2 frames/tick + yield | Yes — no dequeue-induced WDT |
| `225b40a` | Enable ESP-IDF coredump in sdkconfig | No — TG1WDT bypasses panic handler |
| `225b40a` | Defer `autoSaveCheck()` out of dequeue | Unverified — likely helps |
| `225b40a` | `yield()` after SSID lookup in dequeue | Unverified — likely helps |
| `225b40a` | `yield()` in LOCKING state | Unverified — speculative |

## Fixes proposed but NOT applied (don't know if they help)

- Bigger TX buffer pool (256 entries) — costs internal RAM, unverified
- Custom WDT timeout handler that logs PC before restart
- Stack size increase for the loop task (currently default ~8KB)
- `std::atomic<bool>` for `oinkBusy` (currently `volatile bool`,
  racy across cores)
- Reserve `handshakes` vector to `MAX_HANDSHAKES` upfront to
  prevent reallocation invalidating references

## What's actually needed to stop guessing

**Capture the PC at the moment of the WDT fire.** The only reliable
way is a custom watchdog handler. ESP-IDF's `task_wdt` API doesn't
expose a timeout callback, but we can:

1. Install a periodic timer interrupt that runs at a low priority
   and snapshots the main task's PC into RTC memory every second
2. After reboot, read RTC memory and print the PC before the next
   iteration

This is the **next concrete step** if we want to actually debug the
TG1WDT instead of guessing.

## How to read this report

- "Verified" = the change is observed in test logs to have the
  intended effect
- "Unverified" = the change was made on a plausible hypothesis but
  no test log yet confirms it
- "Speculative" = applied without strong evidence, may or may not
  help

The user is correct that we have been adding fixes "just for the
luck". The remaining unfixed crash is a real bug; we need
observability before more code changes.
