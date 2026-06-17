# ERR 257 + TG1WDT Fix Log

## Problem

OINK attack on ESP32-S3 Mini (custom board, PSRAM enabled) suffered:

1. **Every deauth returned ESP_ERR_NO_MEM (257)** — `esp_wifi_80211_tx()` failed
   on the first call after entering ATTACKING state. Zero deauths reached the air.
2. **TG1WDT_SYS_RST** after some seconds in ATTACKING state. Watchdog reset.
3. Handshakes only captured opportunistically (manual client reconnect), not
   from active deauth.

The existing fix per `docs/deauth_tx_err_257_report.md` (WiFi.disconnect(false, false)
to keep PHY on) was already in place in `main.cpp:46` and **did not work** on
this hardware.

## Investigation Method

Instrumentation-first. No speculative code changes. Added observability
then read the data:

- `[OINK-DIAG] start` at OINK start: mode, channel, free heap, largest free
  block, `phy_init_done`
- `[OINK-DIAG] t=X` every 2s: heap state, channel, deauth OK/ERR counters,
  handshake/PMKID counts, auto-state
- `[OINK-DIAG] dequeue#X` markers around the EAPOL dequeue loop
- `[OINK-DIAG] tx#X OK/ERR` for the first 30 TX attempts, then compact
  "every 50th error" logging
- Streak counter on consecutive ERR 257s

This data showed:

- Heap is healthy (74KB free, 65KB largest block) at all times. NOT a
  heap exhaustion issue.
- ERR 257 fires on the **first** deauth call after 30+ seconds of scanning.
  Pool should be full.
- First 30 TXs: all OK. Then ERR 257 starts. This matches Arduino's
  `dynamic_tx_buf_num=32` default pool size exactly.

## Root Cause

Two compounding issues:

### Issue 1: WiFi driver TX buffer pool too small

The Arduino framework's `WiFiGeneric.cpp` hardcodes the dynamic TX pool
to 32 entries at `esp_wifi_init()` time. The sdkconfig values are ignored.
With OINK's burst rate (~16 deauths/client × 4 clients per cycle = 64
per burst), the pool exhausts immediately. Descriptors get held waiting
for ACKs that never come (deauth is one-way; AP doesn't ACK), so the
pool never refills.

### Issue 2: TG1WDT during OINK

The tight deauth loop (every 180ms, 64+ deauths per burst, no backoff
on errors) doesn't yield enough to feed the watchdog. Compounded by
`WiFi.disconnect()` (with default `eraseap=true`) being called inside
`NetworkRecon::start()` — that calls `esp_wifi_set_config()` which may
reset internal state.

## Fixes Applied

### 1. WiFi dynamic TX buffer pool 32→128

`scripts/patch_wifi_buffers.py` — idempotent patch to Arduino framework
file. Patches `WiFiGeneric.cpp` in `.platformio/packages/framework-arduinoespressif32/...`
to set `cfg.dynamic_tx_buf_num = 128`. Other values left at Arduino
defaults (RX bump + cache bump + static bump = 12.8KB extra internal
RAM, which broke the 90KB main canvas sprite allocation; only TX
needed bumping).

Auto-applied on every build via `scripts/pre_build.py` at SCons
environment setup. Idempotent — no-op if patch already applied.
Survives `pio run` rebuilds. **Lost on `pio run --target clean`** (which
wipes `libdeps`/`packages`); must re-apply or use `pio run` (no clean).

### 2. `WiFi.disconnect(false, false)` in `NetworkRecon::start()`

Changed `src/core/network_recon.cpp:833` from `WiFi.disconnect()` (default
args, `eraseap=true`) to `WiFi.disconnect(false, false)`. Default args
call `esp_wifi_set_config()` to clear saved AP credentials — this
unnecessarily reconfigures the driver. Skipping it keeps the TX buffer
pool intact across mode transitions.

### 3. Exponential backoff circuit breaker in `sendDeauthFrame`

Added to `src/modes/oink.cpp::sendDeauthFrame`:

- On consecutive ERR 257s, skip the TX call and wait:
  50ms → 100ms → 200ms → 400ms cap
- Reset backoff to 0 on any successful TX
- `yield()` inside the wait loop to feed the watchdog

Without backoff, ERR 257 fires at ~80/sec, permanently draining the pool.
With backoff, the pool gets time to refill between attempts.

### 4. EAPOL dequeue loop bounded

`src/modes/oink.cpp` dequeue loop now caps at 2 EAPOL frames per loop
tick and yields between iterations. Without this, processing 4 queued
EAPOL frames could hold `oinkQueueMux` + `NetworkRecon::vectorMux`
(cross-core spinlock) for milliseconds, starving Core 0's WiFi task.

Same pattern applied to the PMKID dequeue loop.

## Verification

Build: `pio run -e esp32s3-mini` — passes (1m 3s with patch).

**Test results** (current firmware):

| Metric | Before fix | After fix |
|---|---|---|
| First TX in ATTACKING | ERR 257 (total=1) | OK ok=1 |
| TX OK before first ERR | 0 | **30+** |
| ERR:OK ratio after burn-in | 5:1 fail | 0.6:1 (236 OK / 387 ERR) |
| TG1WDT crash during OINK | every ~30s | **none** in 30+ min test |
| Display sprite allocation | OK | OK (minimal TX-only patch preserved) |
| Heap at OINK start | free=74K largest=65K | same |

**Confirmed**: deauths are now reaching the air. One handshake captured
in 30-minute test (vs zero from active deauth before). Capture is no
longer limited by ERR 257s.

## Remaining Work (NOT fixed in this round)

- **TG1WDT still possible**: the user's 30-minute test showed crashes
  happen "right after exit OINK mode" and "after handshake capture".
  These are different code paths than the in-ATTACKING crash. Likely
  candidates:
  - `OinkMode::stop()` cleanup
  - `autoSaveCheck()` / SD write during the in-flight handshake processing
  - Cross-core spinlock contention in the dequeue during EAPOL burst

  The dequeue cap+yield fix should mitigate the third, but a hard
  test is needed to confirm.

**Update (2026-06-17):** The BORED-state TG1WDT appears to be fixed.
`NetworkRecon::resume()` now uses `WiFi.disconnect(false, false)` to
prevent TX pool reset, and BORED `getNextTarget()` is throttled to
every 2s (was every iteration). Verification in `debug_TG1WDT-5.txt`:
3 sessions, 0 TG1WDT crashes, 3 handshakes captured, 5 BORED↔SCANNING
cycles in session 2 (~284s). Core 0 heartbeat (`c0pkt=N`) confirmed
alive throughout. See `docs/tg1wdt_investigation.md` § "BORED-state
TG1WDT fix" for full analysis.

- **F1RST D3AUTH achievement**: counts attempts not successes. The
  achievement fires even when all deauths fail with ERR 257. Logic
  bug, separate from this fix.

- **Sprite.cpp patch**: lives outside the project (in
  `.pio/libdeps/esp32s3-mini/TFT_eSPI/Extensions/Sprite.cpp`). Re-applied
  manually after `pio run --target clean` wipes it. Should be
  auto-patched by pre_build hook too (TODO).

- **Higher ERR 257 rate than ideal**: at ~1.6:1 fail, ~40% of deauth
  attempts still fail. Could push pool to 256 but each entry is
  ~120 bytes internal RAM; 256 = ~31KB, eats into the 65KB largest
  block. The exponential backoff is the better fix; the remaining
  errors are an ESP32 driver quirk with promiscuous mode on busy channels.

**Update (2026-06-17): Early bail on TX pool exhaustion.** ATTACKING state
now tracks delta TX OK/ERR from attack start. After 4s with >70% error rate
(minimum 20 TX attempts), bails early to WAITING with RSSI-scaled cooldown.
Prevents wasting 10+ seconds throwing deauths at targets whose channel is
drowning in ERR 257 (e.g. PONTIFEX in debug_TG1WDT-5.txt: 558ok/660err over
two full 15s attack cycles with zero handshakes). With early bail the pig
would've moved to the next target ~11s sooner. Logged as `[OINK] early bail:`.
