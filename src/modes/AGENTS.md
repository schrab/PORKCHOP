# src/modes — Operating Modes

## Purpose

12 independent operating modes. Each has init/start/stop/update lifecycle, owns its own rendering and input handling, and dispatches from the central state machine in `porkchop.cpp`.

## Ownership

- `oink.*` — Deauth + handshake capture (primary attack mode)
- `donoham.*` — Passive recon (DO NO HAM, zero TX)
- `warhog.*` — Wardriving (GPS + CSV logging)
- `bacon.*` — Beacon injection (hide & seek)
- `spectrum.*` — WiFi spectrum analyzer + client monitor + attack mode (PMKID + full handshake capture)
- `piggyblues.*` — BLE notification spam
- `pigsync_client.*` — ESP-NOW peer sync
- `pigsync_protocol.h` — PigSync protocol definitions (header only)
- `charging.*` — Low-power battery display
- `pork_patrol.*` — Flock Safety / Axon bodycam / tracker detection
- `swine_radar.*` — 5-tab threat radar
- `snout.*` — Evil twin + deauth storm + hidden SSID prober

## Local Contracts

### Mode lifecycle
- `init()` — one-time setup (static, called once)
- `start()` — begin mode operations
- `stop()` — cleanup, release resources
- `update()` — called every loop iteration while mode is active

### Packet callback exclusivity
- `NetworkRecon::setPacketCallback()` supports only **one** callback at a time.
- SnOUT registers deauth callback on start, clears on stop.
- **Do not run SnOUT alongside modes that use packet callbacks** (OINK, SPECTRUM).

### Core 0 spinlock safety (TG1WDT prevention)
- OINK and SPECTRUM callbacks run on **Core 0** (WiFi task context).
- `NetworkRecon::enterCritical()` acquires `vectorMux` — a cross-core spinlock.
- **Core 0 callbacks MUST NEVER call `NetworkRecon::enterCritical()`** — if Core 1 holds
  vectorMux, Core 0 blocks inside `taskENTER_CRITICAL` with interrupts disabled → TG1WDT.
- **OINK callback is a PURE ENQUEUER**: `processEAPOL()` only copies raw frame data to
  `pendingHsPool[]` (per-frame slots, no vector iteration). It NEVER accesses
  `handshakes[]` or `pmkids[]` vectors. Core 1 dequeue does ALL vector lookups and writes.
- `oinkQueueMux` ONLY protects `pendingHsPool[]` (the queue). Safe functions
  (`findOrCreateHandshakeSafe`/`findOrCreatePMKIDSafe`) do NOT use it — they run on
  Core 1 only where the callback never touches those vectors.
- SPECTRUM uses `spectrumQueueMux` (or equivalent mode-owned spinlock) for its queue.
- Core 1 main thread code accesses `networks[]` via `NetworkRecon::enterCritical()`
  (safe on Core 1) with `oinkBusy=true` to gate Core 0 callbacks.
- SSID lookups from `networks[]` are **deferred** from Core 0 to Core 1 dequeue handlers.

### NetworkRecon interaction modes
| Mode | NetworkRecon action |
|---|---|
| OINK | Uses shared networks, registers packet callback |
| DO NO HAM | Uses shared networks, no callback (passive) |
| WARHOG | Uses shared networks, no callback |
| SPECTRUM | Uses shared networks, registers packet callback |
| PIGGYBLUES | `stop()` WiFi entirely (BLE needs WiFi OFF) |
| PIGSYNC | `pause()` WiFi (ESP-NOW conflicts with promiscuous) |
| SNOUT | Registers deauth callback on start |
| FILE_TRANSFER | `freeNetworks()` to release memory |

### Input handling
- Modes use `hal_input_wasPressed()` for discrete presses.
- `hal_input_shouldExit()` checks ESC pressed.
- LEFT/RIGHT for tier/category cycling. UP/DOWN for list navigation.
- ENTER/SELECT for primary action.
- SPECTRUM uses long-press: `hal_input_isLongUp()` (enter attack mode), `hal_input_isLongRight()` (filter cycle), `hal_input_isLongEnter()` (reveal mode). Long-press checks must run **before** short-press one-shot guards.

### Data capture
- OINK: handshakes (PCAP + hashcat 22000) + PMKIDs saved to SD (`/porkchop/handshakes/`)
- SPECTRUM attack mode: PMKIDs + full 4-way handshakes (PCAP + hashcat 22000) saved to SD (same dir as OINK)
- WARHOG: GPS + network CSV to SD (`/porkchop/wardriving/`)
- Captures are low-frequency SD writes (acceptable)

## Verification

Build: `pio run -e esp32s3-mini`

| Mode | Verify |
|---|---|
| OINK | Scans networks, selects target, deauths, captures handshake |
| DO NO HAM | Passively lists networks, zero TX |
| WARHOG | Logs GPS + networks to CSV |
| SNOUT | Detects evil twin, monitors deauth storm |
| PIGSYNC | ESP-NOW peer discovery + sync |
| BACONTX | Broadcasts beacon frames |
| SPECTRUM | Displays channel activity, client monitor, attack mode captures handshakes |
| PORK PATROL | Detects Flock/Axon signals |
| SWINE RADAR | Shows 5-tab threat data |
| PIGGYBLUES | BLE notification spam |
| CHARGING | Low-power battery display |
| WEBUI | Browser screen mirror active |
