# PORKCHOP — ESP32-S3 Mini Port

# Karpathy Guidelines

Behavioral guidelines to reduce common LLM coding mistakes.

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

## Overview

PORKCHOP is a WiFi pentesting firmware for ESP32-S3 microcontrollers.  
Originally built for M5Cardputer (ESP32-S3 + keyboard + display).  
This branch ports it to a **custom ESP32-S3 Mini board** with:

- ST7789 320×170 display (landscape)
- 5-way joystick (UP/DOWN/LEFT/RIGHT/SELECT)
- GPS (UART, TX=1 RX=2)
- SD card (SPI2_HOST)
- NeoPixel (GPIO 33)
- Battery ADC (GPIO 8)
- Passive piezo buzzer (GPIO 7)

## Architecture

```
src/
├── main.cpp                  Entry point — HAL init + Porkchop loop
├── core/                     Core state machine + config + XP
│   ├── porkchop.h/.cpp       Main state machine (mode dispatch)
│   ├── config.h/.cpp         Persistent config (JSON on SD/SPIFFS)
│   ├── xp.h/.cpp             RPG XP / leveling system
│   ├── challenges.h/.cpp     Session challenges
│   ├── network_recon.h/.cpp  Background WiFi scanning
│   ├── ghost.h/.cpp          MAC randomization (Ghost Mode)
│   ├── wartales.h/.cpp       Session diary (WarTales)
│   ├── heap_*.h/.cpp         Heap management & diagnostics
│   ├── sd_format.h/.cpp      SD card format utilities
│   └── ...
├── hal/                      Hardware Abstraction Layer
│   ├── hal_pins.h            Pin definitions (ESP32-S3 Mini)
│   ├── hal_display.h/.cpp    TFT_eSPI + DisplayCanvas (M5Canvas wrapper)
│   ├── hal_input.h/.cpp      5-way joystick debounced input
│   ├── hal_audio.h/.cpp      Piezo buzzer via LEDC PWM
│   ├── hal_battery.h/.cpp    Battery ADC voltage reading
│   ├── hal_neopixel.h/.cpp   NeoPixel control
│   ├── hal_rtc.h/.cpp        Internal RTC
│   ├── hal_imu.h/.cpp        IMU stub (no hardware on Mini)
│   └── hal_board.h/.cpp      Board type detection
├── ui/                       Display drawing
│   ├── display.h/.cpp        3-canvas sprite system (topBar/mainCanvas/bottomBar)
│   ├── menu.h/.cpp           Navigation menu (6 groups: ATTACK/RECON/LOOT/COMMS/RANK/SYSTEM)
│   ├── captures_menu.h/.cpp  Handshake capture viewer
│   ├── settings_menu.h/.cpp  Configuration UI
│   └── ... (13 UI menus)
├── modes/                    Operating modes
│   ├── oink.h/.cpp           Deauth attack mode
│   ├── donoham.h/.cpp        Passive recon mode (DO NO HAM)
│   ├── warhog.h/.cpp         Wardriving mode (CSV logging)
│   ├── bacon.h/.cpp          Beacon injection mode
│   ├── spectrum.h/.cpp       WiFi spectrum analyzer + client monitor + attack mode
│   ├── piggyblues.h/.cpp     BLE advertisement spam
│   ├── pigsync_client.h/.cpp ESP-NOW peer sync
│   ├── charging.h/.cpp       Low-power battery display
│   ├── pork_patrol.h/.cpp    Flock Safety + Axon bodycam detection
│   ├── swine_radar.h/.cpp    5-tab threat detection radar
│   ├── snout.h/.cpp          Evil twin + deauth storm + hidden SSID prober
│   └── (12 modes total)
├── piglet/                   Personality system
│   ├── avatar.h/.cpp         ASCII pig avatar
│   ├── mood.h/.cpp           Mood/phrase system
│   └── weather.h/.cpp        Weather animation
├── audio/sfx.h/.cpp          Sound effect engine (queued, non-blocking)
├── gps/gps.h/.cpp            GPS parsing (TinyGPSPlus)
└── web/                      HTTP file server + Wigle upload
    ├── fileserver.h/.cpp
    ├── serial_api.h/.cpp    Serial API key entry (WIGLE/WPASEC)
    ├── wigle.h/.cpp
    ├── wpasec.h/.cpp
    └── webui.h/.cpp           Browser screen mirror + remote control
```

## Build

```bash
# Build
pio run -e esp32s3-mini

# Clean + rebuild
pio run -e esp32s3-mini --target clean && pio run -e esp32s3-mini

# Upload (always use explicit port if first attempt times out)
pio run -e esp32s3-mini --target upload --upload-port COM35
```

## Modes

### ATTACK Group
| Mode | Menu ID | Description |
|------|---------|-------------|
| OINK | 1 | Deauth + handshake capture (primary attack mode) |
| PIGGYBLUES | 8 | BLE notification spam |
| SNOUT | 25 | Evil twin detection, deauth storm monitoring, hidden SSID prober |

### RECON Group
| Mode | Menu ID | Description |
|------|---------|-------------|
| DO NO HAM | 14 | Passive recon (zero TX, pure listening) |
| WARHOG | 2 | Wardriving — GPS + CSV logging |
| SPECTRUM | 10 | WiFi spectrum analyzer |
| PORK PATROL | 22 | Flock Safety / Axon bodycam / tracker detection |
| SWINE RADAR | 23 | 5-tab threat radar (cellular/Drones/Tags/Stingrays/Skimmers) |

### LOOT Group
| Mode | Menu ID | Description |
|------|---------|-------------|
| CAPTURES | 4 | View captured handshakes/PMKIDs |
| TRACKS | 13 | WiGLE trail viewer |
| BOUNTY | 17 | Active bounty tracker |

### COMMS Group
| Mode | Menu ID | Description |
|------|---------|-------------|
| PIGSYNC | 16 | ESP-NOW peer sync |
| BACONTX | 18 | Beacon broadcast (hide & seek) |
| TRANSFR | 3 | WiFi file transfer |
| WEBUI | 24 | Browser screen mirror + remote control |

### RANK Group
| Mode | Menu ID | Description |
|------|---------|-------------|
| FLEXES | 9 | Session achievements |
| BADGES | 11 | Lifetime achievements |
| UNLOCK | 15 | Secret challenges |

### SYSTEM Group
| Mode | Menu ID | Description |
|------|---------|-------------|
| SETTINGS | 5 | Configuration |
| BOARBROS | 12 | Excluded networks |
| COREDUMP | 7 | Crash viewer |
| DIAGDATA | 19 | System diagnostics |
| FORMATSD | 20 | SD card format |
| CHARGING | 21 | Low-power charging display |
| ABOUTPIG | 6 | About / credits |

## Core Services

### Ghost Mode (`core/ghost.h/.cpp`)
MAC randomization service. Generates locally-administered MACs via `esp_random()`.
Applied at boot if `ghostEnabled` persists in config. Saves/restores real MAC.

### WarTales (`core/wartales.h/.cpp`)
Session diary with `[MM:SS]` uptime timestamps. Open file handle per session,
flushes every 10 events or 10s. `logEvent()` records mode starts and key actions.
`logCapture()` / `logDetection()` for OinkMode and PorkPatrol events.

### NetworkRecon (`core/network_recon.h/.cpp`)
Background WiFi scanning service. Channel hopping, stale network cleanup,
promiscuous packet callback (one at a time).

**Thread safety**:
- `enterCritical()` / `exitCritical()` protect the shared `networks[]` vector.
  These are safe on Core 1 (main thread) but **MUST NOT** be called from Core 0
  (promiscuous callback) — cross-core spinlock deadlock → TG1WDT.
- Mode-owned capture data (OINK `handshakes[]`, `pmkids[]`) is **Core 1 territory**.
  The callback is a PURE ENQUEUER — copies raw frame data to `pendingHsPool[]`,
  no vector iteration. Core 1 dequeue does all vector lookups and writes.

## Display Port Status

**See full technical report**: `docs/display_port_report.md`

### Working
- ✅ Full 320×170 resolution, ST7789 driver
- ✅ SPI3_HOST (HSPI) via `USE_HSPI_PORT` in User_Setup.h
- ✅ Colors: BGR order, inversion on
- ✅ UI boots and responds to input
- ✅ SPI stable at 40 MHz

### Not Working
- ❌ **180° rotation**: MX and MY bits in MADCTL register have **no effect** on this display module. Only MV bit works. The orientation is fixed at hardware level.
- ❌ **Vertical shift**: CGRAM offset shifts content up 35px, leaving unwritten area at bottom.

### Fixed
- ✅ **Sprite corruption**: Fixed by embedding TFT_eSprite by value (not pointer) in DisplayCanvas + moving char buffers to `.ext_ram.bss` section.
- ✅ **Dim wake**: Fixed by calling `Display::resetDimTimer()` on any key state change in `porkchop.cpp`.

### Key Display Config (User_Setup.h)
```cpp
#define USER_SETUP_LOADED
#define ST7789_DRIVER
#define CGRAM_OFFSET                     // Enables rowstart/colstart in rotation code
#define TFT_WIDTH  170                   // Portrait native — rotation swaps to 320x170
#define TFT_HEIGHT 320
#define USE_HSPI_PORT                    // SPI3_HOST (correct bus)
#define SPI_FREQUENCY  40000000          // 40 MHz — matches WiFiTool
#define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_ON
// In hal_display_init(): g_Display.setRotation(3);  // MV|MY|BGR=0xA8
```

## HAL API Reference

### Display (`hal_display.h`)
```cpp
extern TFT_eSPI g_Display;           // Global TFT driver
void hal_display_init();             // Init + set rotation
void hal_display_setBrightness(uint8_t brightness);  // LEDC PWM on BL pin

// M5Canvas-compatible sprite wrapper
class DisplayCanvas {
    void createSprite(w, h, bpp=16);   // Create off-screen sprite at given color depth
    void deleteSprite();
    void pushSprite(x, y);           // Flush sprite to display
    void fillSprite(color);
    void fillScreen(color);
    void setTextColor(fg, bg=...);
    void setTextSize(float);
    void setTextDatum(uint8_t);      // top_left=0, middle_center=4, etc.
    void setFont(const GFXfont*);
    void drawString(str, x, y);
    void drawCentreString(str, x, y);
    int16_t width(), height();
    int16_t textWidth(str);
    void print(int/float/str);       // M5Canvas compat
    // ... fillRect, drawRect, fillCircle, drawLine, etc.
};
```

### Input (`hal_input.h`)
```cpp
// Key codes
KEY_UP 0xDA  KEY_DOWN 0xD9  KEY_LEFT 0xD8  KEY_RIGHT 0xD7
KEY_ENTER 0x0D  KEY_ESC 0x1B

void hal_input_init();         // Setup joystick GPIOs (INPUT_PULLUP)
void hal_input_update();       // Poll + debounce — call once per loop()
bool hal_input_isChange();     // Any state changed since last update?
bool hal_input_wasPressed(uint8_t keyCode);  // Was specific key just pressed?
bool hal_input_anyHeld();      // Is any key currently held?
bool hal_input_isPressed();    // Alias for anyHeld()
void hal_input_waitRelease();  // Block until all keys released
uint8_t hal_input_getch();     // Get next buffered key
InputEvent hal_input_keysState();  // Single-shot poll {pressed, key}
bool hal_input_isKeyPressed(char key);  // Raw read of specific key
bool hal_input_shouldExit();   // ESC pressed?
bool hal_input_isLongEnter();  // Long-press ENTER (800ms hold)
bool hal_input_isLongUp();     // Long-press UP (800ms hold, attack mode in Spectrum)
bool hal_input_isLongRight();  // Long-press RIGHT (800ms hold, filter cycle in Spectrum)
void hal_input_consumeLongEsc();  // Consume long-press LEFT ESC flag
```

### Audio (`hal_audio.h`)
```cpp
void hal_audio_init();         // Setup LEDC timer on piezo pin
void hal_audio_play(uint16_t freq, uint32_t duration_ms);
void hal_audio_stop();
void hal_audio_beep();         // Short notification beep
void hal_audio_click();        // UI click sound
```

### Battery (`hal_battery.h`)
```cpp
void hal_battery_init();       // Setup ADC
uint32_t hal_battery_read_mv();      // Voltage in mV
uint8_t hal_battery_read_percent();  // 0-100%
bool hal_battery_isCharging();       // Charging detect (stub)
```

### RTC (`hal_rtc.h`)
```cpp
struct hal_rtc_datetime_t { uint16_t year; uint8_t month, day, hour, minute, second; };
void hal_rtc_init();
void hal_rtc_getDateTime(hal_rtc_datetime_t* dt);
uint32_t hal_rtc_getUnixTime();
```

### IMU (`hal_imu.h`)
```cpp
bool hal_imu_init();                // Returns false (no IMU on Mini)
bool hal_imu_getAccel(float* x, float* y, float* z);  // Stub: x=0, y=0, z=1.0
bool hal_imu_isAvailable();         // Check if IMU hardware present
```

## Porting Notes (M5Cardputer → ESP32-S3 Mini)

### What was replaced
| M5 API | HAL replacement | Files affected |
|---|---|---|
| `M5Cardputer.Keyboard.*` | `hal_input_*` | porkchop.cpp, modes, UI menus |
| `M5.Display.*` | `g_Display.*` / `DisplayCanvas` | display.cpp, modes |
| `M5.Power.*` | `hal_battery_*` | mood.cpp, charging.cpp, etc. |
| `M5.Rtc.getDateTime()` | `hal_rtc_getDateTime()` | avatar.cpp, mood.cpp |
| `M5.Imu.getAccel()` | `hal_imu_getAccel()` | spectrum.cpp |
| `M5.Speaker.*` | `hal_audio_*` | sfx.cpp |
| `M5Canvas` | `DisplayCanvas` | Every drawing file |
| `M5.update()` | `hal_input_update()` | main.cpp, blocking loops |
| `fonts::Font0` | `NULL` (default font) | display.cpp, crash_viewer.cpp |

### Key mappings
| M5Cardputer key | ESP32-S3 Mini joystick |
|---|---|
| KBD `;` (UP) | Joystick UP (GPIO 40) |
| KBD `.` (DOWN) | Joystick DOWN (GPIO 39) |
| KBD `,` (LEFT) | Joystick LEFT (GPIO 41) |
| KBD `/` (RIGHT) | Joystick RIGHT (GPIO 42) |
| KBD `ENTER` | Joystick SELECT (GPIO 38) |
| KBD `` ` `` (ESC) | SELECT held 1s |

### Display layout (320×170)
```
TOP_BAR     = 14px   (status + notifications)
MAIN_AREA   = 142px  (mode content)
BOTTOM_BAR  = 14px   (bottom overlay)
```

## Convention

- **Commits**: One commit per meaningful phase (no batching)
- **Branch**: `esp32-s3-mini-port` for this port — `main` is upstream M5Cardputer
- **Style**: Arduino framework, C++17, snake_case for functions, PascalCase for classes
- **Config**: New config fields append at end of `ConfigBlob` struct (packed, `CONFIG_VERSION=1`). Old blobs zero-initialize new fields via `memset`.
- **Core 0 spinlock safety**: The WiFi promiscuous callback runs on Core 0. `taskENTER_CRITICAL` on a cross-core spinlock blocks Core 0 with interrupts disabled. If Core 1 holds the lock → TG1WDT after ~300ms. **OINK callback is a PURE ENQUEUER**: it only copies frame data to a queue slot under `oinkQueueMux`, no vector access. Core 1 dequeue does ALL vector lookups/writes. `oinkQueueMux` ONLY protects the queue, not vectors.
- **Deferred SSID lookup**: `networks[]` lookups from `networks()` require `vectorMux` (via `NetworkRecon::enterCritical()`). Callback code MUST defer SSID lookups to Core 1 dequeue handlers; never access `networks()` from Core 0.
- **SnOUT note**: `NetworkRecon::setPacketCallback()` supports only **one** callback at a time. SnOUT registers its deauth callback on start, clears on stop. Do not run SnOUT alongside modes that use packet callbacks.
- **Sprite buffer PSRAM trap**: `CONFIG_SPIRAM_USE_MALLOC=y` in sdkconfig makes plain `calloc()` return PSRAM even when TFT_eSPI's `PSRAM_ENABLE` flag is `false`. The library is patched in `.pio/libdeps/.../TFT_eSPI/Extensions/Sprite.cpp` to use `heap_caps_calloc(MALLOC_CAP_INTERNAL)` in all 4 bpp paths of `callocSprite()`. This forces sprite pixel buffers into internal RAM regardless of heap config, preventing PSRAM bus stall TG1WDT during dual-core access (OINK handshake capture + display update). Patch persists across rebuilds; lost only on `pio lib update`.

# DOX framework

- DOX is highly performant AGENTS.md hierarchy installed here
- Agent must follow DOX instructions across any edits

## Core Contract

- AGENTS.md files are binding work contracts for their subtrees
- Work products, source materials, instructions, records, assets, and durable docs must stay understandable from the nearest applicable AGENTS.md plus every parent AGENTS.md above it

## Read Before Editing

1. Read the root AGENTS.md
2. Identify every file or folder you expect to touch
3. Walk from the repository root to each target path
4. Read every AGENTS.md found along each route
5. If a parent AGENTS.md lists a child AGENTS.md whose scope contains the path, read that child and continue from there
6. Use the nearest AGENTS.md as the local contract and parent docs for repo-wide rules
7. If docs conflict, the closer doc controls local work details, but no child doc may weaken DOX

Do not rely on memory. Re-read the applicable DOX chain in the current session before editing.

## Update After Editing

Every meaningful change requires a DOX pass before the task is done.

Update the closest owning AGENTS.md when a change affects:

- purpose, scope, ownership, or responsibilities
- durable structure, contracts, workflows, or operating rules
- required inputs, outputs, permissions, constraints, side effects, or artifacts
- user preferences about behavior, communication, process, organization, or quality
- AGENTS.md creation, deletion, move, rename, or index contents

Update parent docs when parent-level structure, ownership, workflow, or child index changes. Update child docs when parent changes alter local rules. Remove stale or contradictory text immediately. Small edits that do not change behavior or contracts may leave docs unchanged, but the DOX pass still must happen.

## Hierarchy

- Root AGENTS.md is the DOX rail: project-wide instructions, global preferences, durable workflow rules, and the top-level Child DOX Index
- Child AGENTS.md files own domain-specific instructions and their own Child DOX Index
- Each parent explains what its direct children cover and what stays owned by the parent
- The closer a doc is to the work, the more specific and practical it must be

## Child Doc Shape

- Create a child AGENTS.md when a folder becomes a durable boundary with its own purpose, rules, responsibilities, workflow, materials, or quality standards
- Work Guidance must reflect the current standards of the project or user instructions; if there are no specific standards or instructions yet, leave it empty
- Verification must reflect an existing check; if no verification framework exists yet, leave it empty and update it when one exists

Default section order:
- Purpose
- Ownership
- Local Contracts
- Work Guidance
- Verification
- Child DOX Index

## Style

- Keep docs concise, current, and operational
- Document stable contracts, not diary entries
- Put broad rules in parent docs and concrete details in child docs
- Prefer direct bullets with explicit names
- Do not duplicate rules across many files unless each scope needs a local version
- Delete stale notes instead of explaining history
- Trim obvious statements, repeated rules, misplaced detail, and warnings for risks that no longer exist

## Closeout

1. Re-check changed paths against the DOX chain
2. Update nearest owning docs and any affected parents or children
3. Refresh every affected Child DOX Index
4. Remove stale or contradictory text
5. Run existing verification when relevant
6. Report any docs intentionally left unchanged and why

## User Preferences

When the user requests a durable behavior change, record it here or in the relevant child AGENTS.md

## Child DOX Index

| Child | Path | Scope |
|---|---|---|
| core | `src/core/AGENTS.md` | State machine, config, XP, network recon, SD I/O, heap mgmt |
| hal | `src/hal/AGENTS.md` | Hardware abstraction (display, input, audio, battery, NeoPixel, RTC) |
| ui | `src/ui/AGENTS.md` | Display drawing, 3-canvas system, 13 menus, themes, dimming |
| modes | `src/modes/AGENTS.md` | 12 operating modes (attack, recon, comms, system) |
| web | `src/web/AGENTS.md` | HTTP file server, WiGLE/WPA-SEC uploads, WebUI screen mirror |
| piglet | `src/piglet/AGENTS.md` | Personality system (avatar, mood, weather) |

**Not indexed** (no child AGENTS.md needed):
- `src/audio/` — 2 files, simple LEDC wrapper
- `src/gps/` — 2 files, single-purpose TinyGPSPlus wrapper
- `test/` — covered by `test/README.md`
- `docs/`, `boards/`, `scripts/` — static config/reference only