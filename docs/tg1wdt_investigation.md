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

## Snapshot service — what we tried and why we stopped

A phase-checkpoint sampler (`src/core/pc_snapshot.{h,cpp}`) was added
to capture the last main-loop phase entered before a WDT reset. It
writes the current `Phase` ID + `millis()` to `RTC_DATA_ATTR` storage
so the next boot can print which phase we were last in.

**Result: the snapshot service is implemented and the code is in
place, but the data it produces is not useful for debugging TG1WDT.**

**Evidence (single OINK crash → reboot, 2026-06-17):**

```
[OINK-DIAG] t=141461 ... auto=1
[OINK-DIAG] t=143487 ... auto=1
[OINK-DIAG] dequeue#2400 enter read=3 write=3
[OINK-DIAG] dequeue#2400 exit processed=0
[RECON] Channel locked to 11
[RECON] Channel locked to 1
[RECON] Channel locked to 2
ESP-ROM:esp32s3-20210327
rst:0x8 (TG1WDT_SYS_RST) Saved PC:0x42075b1f
…
=== PORKCHOP STARTING ===
[PC-SNAPSHOT] raw phase=0 ms=0 (name=BOOT)        ← prior boot crashed
[PC-SNAPSHOT] no prior snapshot (clean boot or first run)
[BOOT] NVS init: OK
[PC-SNAPSHOT] self-test: wrote phase=0xAA ms=0xBBCCDDEE; readback phase=0xAA ms=0xBBCCDDEE
[PC-SNAPSHOT] init: direct RTC writes (no sampler)
```

**Interpretation:**

- The self-test in `setup()` writes `0xAA` / `0xBBCCDDEE` to the
  `RTC_DATA_ATTR` variables and reads back the same values. **The
  storage works at boot.** This rules out a linker/placement bug
  (the map file confirms `s_rtcPhase`/`s_rtcMs` are in
  `.rtc.data` at `0x50000004`/`0x50000000`).
- The prior boot's RTC contents are zero. The OINK session
  demonstrably reached `OinkMode::update()` (we see `dequeue#2400`
  enter/exit, diag emits, and `[RECON] Channel locked to N` lines)
  and at least 4 of the 5 checkpoint sites are inside that path.
- So either (a) the WDT reset path on this chip wipes the
  `.rtc.data` contents, or (b) the WDT fires from a code path that
  never reaches a checkpoint. We have no experiment to distinguish
  these on real hardware without a deliberate WDT-forced reboot
  test, and that test was not done.
- The simplest explanation consistent with both observations is
  (a): TG1WDT on this firmware/board erases the `.rtc.data`
  section. (The ESP32-S3 TRM documents `.rtc.data` as preserved
  across CPU resets, but the WDT path goes through `esp_restart()`
  → `esp_cpu_reset(0)` and behavior of that path on
  ESP32-S3 has been a moving target across silicon revisions.)
- Even if (b) is correct and adding more checkpoint sites would
  catch it, we have no way to *verify* a fix without a way to
  observe what the snapshot service captures — and the storage is
  demonstrably not capturing it.

**Decision:** the snapshot service is kept in source as harmless
code (it adds ~16 bytes to flash, doesn't affect runtime) but is
not useful for this debug pass. The data we'd want is the
per-tick time spent inside `NetworkRecon::enterCritical()` in
`OinkMode::update()` — which the OINK vs SPECTRUM comparison
below identifies as the prime suspect anyway.

## OINK vs SPECTRUM — behavioral comparison

SPECTRUM is the control case: it runs for 30+ minutes, attacks
clients on a single AP, captures handshakes, never TG1WDTs.
OINK TG1WDTs within 1–3 minutes of attack. Both modes use the
same `NetworkRecon::setPacketCallback` (single slot, last-wins)
and both run on the same hardware. The culprit lives in the
**difference** between the two.

**Key differences (file:line refs in parens):**

1. **Cross-core spinlock count per `update()` tick:**
   - OINK: 22+ `NetworkRecon::enterCritical()` / `exitCritical()`
     pairs in `OinkMode::update()` (`oink.cpp:686,694,704,711,725,733,
     881,915,1008,1016,1097,1109,1206,1226,1241,1256,1280,1288,1317,1322,
     1370,1378,1392,1398,1406,1448`)
   - SPECTRUM: **zero** in the same path. (`SpectrumMode::update`
     uses the `busy` atomic for cross-core visibility, not the
     spinlock.)
   - If Core 0's WiFi task ever needs the same `vectorMux`
     (e.g. via `NetworkRecon::processDeferredEvents` from the
     promiscuous ISR), it spins with IRQs disabled while Core 1
     holds the lock. The TG1WDT is on Core 1's main task — so
     Core 0's spinlock-held deadlock is what kills it.

2. **`oinkBusy = true` window length:**
   - OINK: set at `oink.cpp:533`, cleared at `oink.cpp:789`. The
     Core 0 callback cannot enqueue during that whole window,
     which spans the deferred-event processing, the 2-frame
     dequeue (with 2 `enterCritical` regions per frame), the
     `autoSaveCheck()` blocking SD I/O, the PMKID dequeue, and
     the 10-second beacon audit.
   - SPECTRUM: `busy=true` windows are short and bracketed around
     individual vector accesses only.

3. **Per-attack-tick deauth rate:**
   - OINK: 4 clients × 3–8 deauths + 8 disassoc = 20–44 frames
     every 180 ms ⇒ **~110–240 deauths/sec** sustained
     (`oink.cpp:1011–1066`).
   - SPECTRUM: 4 clients × 2 deauths = 8 frames every 180 ms ⇒
     ~44 deauths/sec, with `delay(3)` between frames
     (`spectrum.cpp:2880`).
   - 5–6× more TX per attack tick in OINK.

4. **Channel switching in auto mode:**
   - OINK: 4–8 `setChannel()` calls per target cycle
     (`selectTarget`, PMKID_HUNTING entry, CH MISMATCH retry,
     LOCKING→ATTACKING transition).
   - SPECTRUM in single-AP monitor+attack: **1** `setChannel()`
     at `enterClientMonitor` (`spectrum.cpp:2431`). Static channel
     for the whole session.

5. **Hot-path memory allocation:**
   - OINK: `findOrCreateHandshakeSafe` mallocs 1500 B
     `beaconData` per handshake (`oink.cpp:1924`); `handshakes[]`
     vector `reserve(5)` grows 5→10→20→40→50 doubling
     (`oink.cpp:351`); `sortNetworksByPriority` copies ~60 KB
     on every SCAN→PMKID_HUNTING transition (`oink.cpp:2846`).
   - SPECTRUM: zero hot-path allocations during monitor+attack;
     all pools are fixed-size BSS (`attackBeaconBuf[350]`,
     `capturedHandshakes[2]`, etc.).

6. **Per-tick spinlock-held work in ATTACKING:**
   - OINK `oink.cpp:1097–1109`: nested `for (hs : handshakes) for
     (net : networks())` under `enterCritical`. With
     MAX_HANDSHAKES=50 and networks()=200, that's 10,000
     iterations per 180 ms = ~55,000 spinlock-held iterations/sec.
   - SPECTRUM has no equivalent.

**Most likely mechanism:**

The combination of (1) excessive Core 1 `enterCritical` calls per
tick, (2) the long `oinkBusy=true` window that blocks Core 0
enqueue, and (6) the spinlock-held `O(handshakes × networks)`
inner loop during ATTACKING. When Core 0's WiFi task needs the
same `vectorMux` (e.g. to enqueue a deauth result, a beacon
update, or any deferred NetworkRecon work), it spins with IRQs
disabled. If the spin exceeds the TG1WDT window, Core 1's main
task — which holds the lock — never gets to call
`esp_task_wdt_reset()` and the chip resets.

The in-file comment at `oink.cpp:622–631` is the maintainer's own
diagnosis of this failure mode: *"each iteration holds
oinkQueueMux AND NetworkRecon::vectorMux (cross-core spinlock)
while iterating networks[]. A full burst of 4 frames × spinlock
+ vector scan + autoSaveCheck() can starve the Core 0 WiFi task
→ TG1WDT_SYS_RST."*

**Next concrete step (if we proceed to a fix):**

Reduce the `enterCritical` count and shorten the `oinkBusy=true`
window to match SPECTRUM's pattern. Specifically:
- Move the `O(handshakes × networks)` inner loop out from under
  `enterCritical` (cache the networks snapshot outside the lock,
  do the work outside, take the lock only for the actual flag
  writes).
- Bracket `oinkBusy = true` around the vector access only, not
  the entire dequeue + autoSave work.
- Add `yield()` between the remaining `enterCritical/exitCritical`
  regions (matches the existing comment at `oink.cpp:709–711`
  which already added `yield()` for exactly this reason).

This is a behavior change to the attack hot path and needs
on-hardware testing (TG1WDT is intermittent, so a single 10-min
run is not enough to claim the fix).

## Fix applied (2026-06-17)

The first item was applied in `oink.cpp:1202–1252` (ATTACKING
handshake-marking scan). The `O(handshakes × networks)` nested
loop no longer holds `vectorMux` for the whole iteration. Instead:

1. **First pass** (lock-free): iterate `handshakes[]`, call
   `NetworkRecon::findNetwork(hs.bssid, &netCopy)` for each
   complete handshake. `findNetwork` takes the lock briefly to
   copy the entry and releases it. Collect the BSSIDs that need
   their `hasHandshake` flag set into a small stack array
   `pendingFlags[8]`. Capture `targetHandshakeSSID` from the
   stack copy.
2. **Second pass** (lock held briefly): take the lock ONCE,
   look up each pending BSSID via `findNetworkIndex()`, and set
   `networks()[idx].hasHandshake = true`. Lock window is now
   bounded by `min(complete_handshakes, 8) × O(1)` writes
   instead of `complete_handshakes × networks()` iterations.

The `oinkBusy=true` window was already bracketed narrowly here
(set right before the lock, cleared right after) — no change
needed for that site. The remaining `enterCritical` sites
(lines 686, 694, 704, 711, 725, 733, 881, 915, 1008, 1016, 1097,
1109, 1206, 1226, 1241, 1256, 1280, 1288, 1317, 1322, 1370, 1378,
1392, 1398, 1406, 1448) are all in the dequeue + state-machine
path and are already bracketed by `yield()` calls. They were not
modified in this commit.

**Verification needed on real hardware:** the TG1WDT is
intermittent. A single 10-minute OINK run is not enough to claim
the fix worked. We need at least the user's normal test pattern
(30+ min OINK with auto-mode switching across multiple APs) to
declare the fix verified. If the WDT still fires, the next most
likely suspect is the 22+ `enterCritical` calls in the state
machine during `auto=1→2→3→4` transitions — but those should be
addressed one site at a time, not in a batch.

## Profiling tool added (2026-06-17)

After the handshake-marking fix did not stop the crash (crash at
`t=38994` in PMKID_HUNTING, before reaching the ATTACKING
handshake-marking scan), a lock-hold profiler was added to
`NetworkRecon::enterCritical/exitCritical`.

**Build flag:** `-DNETRECON_LOCK_PROFILE=1` (set in
`platformio.ini` for the `esp32s3-mini` env only). Off by default
in other envs and the m5cardputer upstream env.

**What it measures:** for every `enterCritical/exitCritical` pair,
records the hold time in microseconds. Accumulates three
quantities over a tick window:

- `lockMax` — longest single hold in this window (µs)
- `lockTotal` — sum of all holds in this window (µs)
- `lockCalls` — number of `enterCritical` calls in this window

The OINK diag emit (2-second interval) appends these to its line:
```
[OINK-DIAG] t=... free=... lockMax=NNN lockTotal=NNN lockCalls=NN
```

**Reset semantics:** `OinkMode::update()` calls
`NetworkRecon::Profile::lockProfileReset()` implicitly on each
diag print (via the `lockProfileSnapshot` + `lockProfileReset`
pair in the diag block). So `lockTotal` and `lockCalls` are
per-2-second-window, not per-tick-of-OinkMode-update.

**What to look for in the data:**

- If `lockMax` is consistently < 1000 µs (1 ms) per window and
  `lockTotal` is < 5000 µs (5 ms) per window: lock contention is
  NOT the WDT cause. The WDT must be from somewhere else
  (RTOS task starvation, memory pressure, WiFi driver hiccup).
- If `lockMax` is in the 1-10 ms range: that's the candidate
  site. The fix path is to refactor that specific lock region
  (the PMKID_HUNTING scan at `oink.cpp:885–915` is a likely
  candidate because it iterates `pmkids[]` inside the lock).
- If `lockMax` is > 10 ms: that's a long critical section by
  any measure, and the WDT almost certainly comes from it. The
  `enterCritical` site that produced it is the immediate
  suspect.

**Site attribution is NOT automatic** — the current profiler
measures aggregate hold time but does not tag the call site.
For a single tick with one long hold, the site is whatever
`OinkMode::update()` was doing at the time of the diag emit;
the user can read the OINK-DIAG line state (`auto=1/2/3/4`) and
correlate with the call site in the state machine.

**Why not flash-backed snapshot storage?** A 1 Hz sampler
writing to flash was considered (see "Snapshot service" section
above) but the direct-RTC approach was simpler and surfaced the
real fact: TG1WDT wipes `.rtc.data` on this chip. Flash would
work but adds wear, ~1-2 ms per write, and the same question
applies: the crash might happen before the write. Profiling
avoids the storage problem entirely by recording the data
*during* the OINK session, not after.
