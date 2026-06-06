# PORKCHOP → ESP32S3_MINI Porting Plan

**Branch:** `esp32-s3-mini-port`
**Base:** PORKCHOP main (M5Cardputer, PlatformIO + Arduino framework)
**Target:** Custom ESP32-S3 Mini board (320×170 ST7789, GPS, SD, 5-way joystick, NeoPixel, battery ADC)
**Reference:** WiFiTool project (proven board_config.cpp, sdkconfig)

---

## 0. Decisions Made

| Question | Decision |
|---|---|
| Framework | PlatformIO + Arduino (keep existing, minimal rewrites) |
| Display lib | TFT_eSPI with M5Canvas-compatible 3-canvas wrapper |
| Audio | Passive piezo on GPIO 7 via ledc PWM |
| Charging mode | Keep (battery ADC from WiFiTool, GPIO 8) |
| PIGGY BLUES (BLE) | Keep (8MB PSRAM available for NimBLE + sprites) |
| PSRAM strategy | 8MB PSRAM → sprites + BLE heap allocated there (like WiFiTool) |
| CYD features to absorb | PORK PATROL, SNOUT MODE, SWINE RADAR, WEBUI |

---

## Phase 1: Build Configuration

### platformio.ini — new `[env:esp32s3-mini]` section

```ini
[env:esp32s3-mini]
platform = espressif32@6.12.0
board = esp32-s3-devkitc-1      ; generic S3, we override below
framework = arduino
board_build.mcu = esp32s3
board_build.f_cpu = 240000000L
board_build.flash_mode = dio
board_build.flash_size = 16MB
board_build.partitions = partitions_esp32s3_mini.csv

; PSRAM: 8MB available, used for sprite buffers + BLE
board_build.psram = enable
board_build.psram_mode = quad
board_build.psram_freq = 40MHz

; Custom sdkconfig for our board
board_build.sdkconfig = sdkconfig.esp32s3_mini

monitor_speed = 115200
upload_speed = 921600

build_flags =
    -DBOARD_ESP32S3_MINI=1
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DCORE_DEBUG_LEVEL=1
    -DBOARD_HAS_PSRAM=1             ; 8MB PSRAM available
    -DPORKCHOP_LOG_ENABLED=0
    -include src/core/logging.h
    ; Display SPI host
    -DDISPLAY_SPI_HOST=SPI3_HOST
    ; NimBLE: use PSRAM for BLE heap (same as M5Cardputer)
    -DCONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=1
    -DCONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=0
    -DCONFIG_BT_NIMBLE_LOG_LEVEL=1
    ; Allow muldefs for raw_frame_sanity_check override
    -Wl,-zmuldefs
    -Os

lib_deps =
    bodmer/TFT_eSPI@^2.5.43
    bblanchon/ArduinoJson@^7.4.2
    mikalhart/TinyGPSPlus@^1.0.3
    h2zero/NimBLE-Arduino@^2.3.7
    ; M5Unified NOT needed — replaced by TFT_eSPI
```

### sdkconfig.esp32s3_mini (new)
Based on PORKCHOP `sdkconfig.defaults` + WiFiTool `sdkconfig` (IDF 5.4.2, 16MB DIO flash at 80MHz, 8MB PSRAM quad at 40MHz):

```
# PORKCHOP proven settings
CONFIG_ESP_COREDUMP_ENABLE=n
CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=2048
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=2048
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_DYNAMIC_FREE_PEER_CERT=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y
CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM=4
CONFIG_ESP32_WIFI_DYNAMIC_RX_BUFFER_NUM=8
CONFIG_ESP32_WIFI_TX_BUFFER_TYPE=1
CONFIG_ESP32_WIFI_DYNAMIC_TX_BUFFER_NUM=16

# Flash
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y

# PSRAM (8MB quad, 40MHz — matches WiFiTool)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_QUAD=y
# CONFIG_SPIRAM_MODE_OCT is not set
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
# CONFIG_SPIRAM_SPEED_120M is not set
# CONFIG_SPIRAM_SPEED_80M is not set
CONFIG_SPIRAM_SPEED_40M=y
CONFIG_SPIRAM_SPEED=40
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_USE_MALLOC=y
```

### partitions_esp32s3_mini.csv (new)
Dual OTA + spiffs (same layout as PORKCHOP, resized for 16MB flash — matching PORKCHOP's existing OTA infrastructure):

```
# Name,   Type, SubType,  Offset,   Size,      Flags
nvs,      data, nvs,      0x9000,   0x6000,
phy_init, data, phy,      0xf000,   0x1000,
factory,  app,  factory,  0x10000,  3M,
ota_0,    app,  ota_0,    ,         3M,
ota_1,    app,  ota_1,    ,         3M,
storage,  data, spiffs,   ,         4M,
```

---

## Phase 2: Hardware Abstraction Layer

### Files to create: `src/hal/`

| File | Purpose |
|---|---|
| `hal_pins.h` | Pin definitions from WiFiTool board_config |
| `hal_display.h/.cpp` | TFT_eSPI init, M5Canvas-compatible wrapper |
| `hal_input.h/.cpp` | 5-way joystick GPIO reading → key events |
| `hal_audio.h/.cpp` | Speaker init (ledc PWM or stub) |
| `hal_battery.h/.cpp` | ADC battery voltage reading |
| `hal_neopixel.h/.cpp` | NeoPixel control |

### Pin Map (from WiFiTool board_config.cpp)

```
DISPLAY (ST7789, SPI3_HOST):
  MOSI=14, SCLK=15, CS=11, DC=12, RST=13, BL=10
  WIDTH=320, HEIGHT=170 (landscape)

SD CARD (SPI2_HOST):
  MOSI=5, MISO=3, SCLK=4, CS=6

JOYSTICK (5-way, digital):
  UP=40, DOWN=39, LEFT=41, RIGHT=42, SELECT=38

GPS (UART):
  TX=1, RX=2

NEOPIXEL:
  DATA=33

BATTERY (ADC):
  VOLTAGE=8

PIEZO BUZZER:
  GPIO=7 (ledc PWM)
```

### Display Wrapper — M5Canvas-compatible API

The entire PORKCHOP UI is built on 3 M5Canvas sprites:
- `Display::topBar` (14px high → scaled to 18px for 320×170)
- `Display::mainCanvas` (107px → scaled to 136px)
- `Display::bottomBar` (14px → 18px)

The wrapper (`DisplayCanvas` class) will expose:
```cpp
class DisplayCanvas {
    TFT_eSprite* sprite;
public:
    void drawString(const char*, int x, int y);     // maps to TFT_eSprite
    void fillSprite(uint16_t color);
    void pushSprite(int x, int y);
    void setTextColor(uint16_t fg, uint16_t bg);
    void setFont(const GFXfont*);
    void fillRect(int x, int y, int w, int h, uint16_t c);
    void setTextSize(float s);
    void drawRect(...);
    void drawFastHLine(...);
    // etc.
};
```

Need to reimplement ~40 M5Canvas/TFT_eSPI calls used across display.cpp.

### Layout Constant Scaling

| Constant | Original (240×135) | Scaled (320×170) |
|---|---|---|
| DISPLAY_W | 240 | 320 |
| DISPLAY_H | 135 | 170 |
| TOP_BAR_H | 14 | 18 |
| BOTTOM_BAR_H | 14 | 18 |
| MAIN_H | 107 | 134 |
| Avatar LEFT | 20 | 27 |
| Avatar RIGHT | 108 | 144 |
| Font size | textSize 3 (18px) | same (readable) |

---

## Phase 3: Main Entry Point Rewrite (`main.cpp`)

Replace M5Cardputer-specific init with HAL:

```cpp
// 1. Serial + delay
// 2. Init SD (SPI2_HOST: MOSI=5, MISO=3, SCLK=4, CS=6)
// 3. Init display (SPI3_HOST: TFT_eSPI)
// 4. Init audio (stub or ledc)
// 5. Init input (joystick GPIO)
// 6. Init battery ADC
// 7. Init NeoPixel
// 8. Init GPS (UART 1: TX=1, RX=2)
// 9. Init WiFi (heap fence same as original)
// 10. Load config from SD
// 11. Init modes, background tasks
// 12. Loop: input → modes → display → GPS
```

Remove:
- `M5Cardputer.begin()` / `M5Cardputer.update()` / `M5.update()`
- `M5.begin()` / `M5.Display.*`
- `M5Cardputer.Keyboard.*`
- `pinMode(0, INPUT_PULLUP)` (replaced by joystick)
- `delay(HeapPolicy::kWiFiModeDelayMs)` — keep WiFi fence

Keep:
- WiFi fence (heap layout reservation)
- All mode init/update calls
- Config loading
- Mood/Avatar system
- HeapHealth & watermarks

---

## Phase 4: Input Handling — 5-Way Joystick

The CYD port already defines 5 touchscreen zones mapped as UP/DOWN/LEFT/RIGHT/SELECT — this same 5-way concept maps directly to our joystick GPIOs. Key event codes stay the same; just replace the touch controller read with GPIO digitalRead.

Map joystick to key events:

| Joystick | Mapped Key | Action |
|---|---|---|
| UP | KEY_UP (0xDA) | Menu up, scroll up, prev network |
| DOWN | KEY_DOWN (0xD9) | Menu down, scroll down, next network |
| LEFT | KEY_LEFT (0xD8) | Back/exit, decrease value |
| RIGHT | KEY_RIGHT (0xD7) | Enter/select, increase value |
| SELECT (press) | KEY_ENTER (0x0D) | Confirm, toggle mode |
| SELECT (hold 1s) | KEY_ESC (0x1B) | Back to idle, cancel |

Add debounce: 50ms minimum interval, digitalRead with pull-up.

Modify `porkchop.cpp` `handleInput()`:
- Replace `M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()` → joystick `readKeys()`
- Map keys to `PorkchopMode` transitions (same logic, different key source)

---

## Phase 5: UI/Layout Adaptation

### display.cpp changes:
- Replace all `M5Canvas` → `DisplayCanvas` (TFT_eSprite wrapper)
- Replace `M5.Display.*` direct calls → TFT_eSPI calls
- Replace color constants `TFT_*` → `PorkTheme` system (already works)
- Update Avatar positioning, menu layout for 320×170
- Scale all text positions, icon positions
- Update top/bottom bar layout

### menu.cpp changes:
- Joystick navigation: UP/DOWN to move, SELECT to confirm
- Scroll indicators for lists longer than visible area

### Other UI files:
- `settings_menu.cpp`, `captures_menu.cpp`, etc. — minimal changes since they draw to the same canvas abstraction

---

## Phase 6: Port CYD New Features

### PORK PATROL → `src/modes/pork_patrol.h/.cpp`
- Extract `namespace PorkPatrol` from CYD .ino (~200 lines)
- Detection: OUI prefixes (Flock Safety + Axon) + SSID patterns
- Matching against NetworkRecon scan data
- UI: FLOCK:X BODYCAM:Y TOTAL:Z display
- Keep as standalone mode

### SNOUT MODE → `src/modes/snout.h/.cpp`
- Extract `namespace SnoutMode` from CYD .ino (~190 lines)
- Evil twin: clone detected AP with same SSID on same channel
- Deauth storm: high-rate deauth bursts
- Hidden SSID prober: probe requests that force APs to reveal hidden SSID
- UI: twin count, storm rate, hidden count

### SWINE RADAR → `src/modes/swine_radar.h/.cpp`
- Extract `namespace SwineRadar` (~180 lines)
- Drone RemoteID: BLE advertisement detection
- AirTag/tracker: BLE pattern detection
- Stingray heuristic: IMSI catcher signature detection
- UI: radar-like visualization

### WEBUI → `src/web/webui.h/.cpp`
- Extract web server from CYD (~190 lines)
- Soft AP mode (simultaneous with STA, or exclusive)
- HTTP server: screen JPEG capture + gesture injection
- Works alongside existing `fileserver.cpp`
- Reachable at `192.168.4.1`

---

## Phase 7: Audio (Piezo Buzzer)

Passive piezo on GPIO 7 driven via ledc PWM:

- `SFX::init()` → ledc timer/channel setup on GPIO 7
- `SFX::play(freq, duration)` → `ledcWriteTone()` for frequency, timer for duration
- `SFX::stop()` → `ledcDetach()` / ledc_write(0)
- Same tone patterns as original SFX (beeps, melodies) — pitch-controlled by ledc frequency

No amplifier needed — direct GPIO drive is fine for a passive piezo (piezo is inductive, not a speaker coil). Just a series resistor (~100Ω) to limit current.

---

## Phase 8: Integration Checklist

- [ ] PlatformIO builds with no errors
- [ ] Display initializes (ST7789, 320×170, landscape)
- [ ] SD card mounts (SPI2_HOST)
- [ ] GPS gets fix (UART)
- [ ] Joystick navigates menu
- [ ] OINK mode scans + deauths
- [ ] DNH mode passive recon
- [ ] SPECTRUM mode live RSSI visualization
- [ ] WARHOG mode wardrives + saves CSV
- [ ] BACON mode beacon injection
- [ ] PIGGY BLUES BLE advertisement spam
- [ ] CHARGING mode battery display
- [ ] PORK PATROL detects Flock/Axon devices
- [ ] SNOUT mode evil twin + deauth storm
- [ ] SWINE RADAR drone/airtag detection
- [ ] WEBUI accessible from browser
- [ ] PIGSYNC peer discovery (ESP-NOW)
- [ ] All 15 themes render correctly
- [ ] XP system levels up
- [ ] Achievements unlock
- [ ] Heap stable after 1hr runtime

---

## Appendices

### A. Files to Create

```
src/hal/hal_pins.h
src/hal/hal_display.h    src/hal/hal_display.cpp
src/hal/hal_input.h      src/hal/hal_input.cpp
src/hal/hal_audio.h      src/hal/hal_audio.cpp
src/hal/hal_battery.h    src/hal/hal_battery.cpp
src/hal/hal_neopixel.h   src/hal/hal_neopixel.cpp
src/modes/pork_patrol.h  src/modes/pork_patrol.cpp
src/modes/snout.h        src/modes/snout.cpp
src/modes/swine_radar.h  src/modes/swine_radar.cpp
src/web/webui.h          src/web/webui.cpp
platformio.ini           [modified - add env]
partitions_esp32s3_mini.csv
sdkconfig.esp32s3_mini
```

### B. Files to Modify

```
src/main.cpp             — replace M5Cardputer init w/ HAL
src/ui/display.h/.cpp    — replace M5Canvas w/ TFT_eSPI wrapper
src/ui/menu.cpp          — joystick navigation
src/core/porkchop.h/.cpp — add new modes to enum + dispatch
src/core/config.h/.cpp   — add board config
src/audio/sfx.h/.cpp     — stub or ledc driver
src/modes/piggyblues.h/.cpp — verify BLE on S3
src/modes/charging.h/.cpp    — use ADC instead of AXP192
```

### C. Files Unchanged

```
src/core/*               — porkchop, xp, challenges, wifi_utils, heap_*,
                           logging, network_recon, oui, sd_format, sd_layout,
                           sdlog, stress_test, wsl_bypasser
src/gps/*                — GPS class stays (just uses different UART pins)
src/piglet/*             — Avatar, Mood, Weather all unchanged
src/modes/oink.h/.cpp    — core WiFi mode, no HW dependency
src/modes/donoham.h/.cpp — same
src/modes/warhog.h/.cpp  — same
src/modes/spectrum.h/.cpp — same
src/modes/bacon.h/.cpp   — same
src/modes/boar_bros.*    — UI file for network exclusion
src/ui/menu/*            — captures, settings, etc. all draw via DisplayCanvas
src/web/fileserver.*     — standalone
src/web/wigle.*          — standalone
src/web/wpasec.*         — standalone
```
