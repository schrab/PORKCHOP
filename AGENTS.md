# PORKCHOP — ESP32-S3 Mini Port

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
│   ├── menu.h/.cpp           Navigation menu
│   ├── captures_menu.h/.cpp  Handshake capture viewer
│   ├── settings_menu.h/.cpp  Configuration UI
│   └── ... (13 UI menus)
├── modes/                    Operating modes
│   ├── oink.h/.cpp           Deauth attack mode
│   ├── donoham.h/.cpp        Passive recon mode
│   ├── warhog.h/.cpp         Wardriving mode (CSV logging)
│   ├── bacon.h/.cpp          Beacon injection mode
│   ├── spectrum.h/.cpp       WiFi spectrum analyzer
│   ├── piggyblues.h/.cpp     BLE advertisement spam
│   ├── pigsync_client.h/.cpp ESP-NOW peer sync
│   ├── charging.h/.cpp       Low-power battery display
│   └── ... (9 modes total)
├── piglet/                   Personality system
│   ├── avatar.h/.cpp         ASCII pig avatar
│   ├── mood.h/.cpp           Mood/phrase system
│   └── weather.h/.cpp        Weather animation
├── audio/sfx.h/.cpp          Sound effect engine (queued, non-blocking)
├── gps/gps.h/.cpp            GPS parsing (TinyGPSPlus)
└── web/                      HTTP file server + Wigle upload
    ├── fileserver.h/.cpp
    ├── wigle.h/.cpp
    └── wpasec.h/.cpp
```

## Build

```bash
# Build
pio run -e esp32s3-mini

# Clean
pio run -e esp32s3-mini --target clean
```

## HAL API Reference

### Display (`hal_display.h`)
```cpp
extern TFT_eSPI g_Display;           // Global TFT driver
void hal_display_init();             // Init + set rotation
void hal_display_setBrightness(uint8_t brightness);  // LEDC PWM on BL pin

// M5Canvas-compatible sprite wrapper
class DisplayCanvas {
    void createSprite(w, h);         // Create off-screen sprite
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
KEY_ENTER 0x0D  KEY_ESC 0x1B  KEY_BACKSPACE 0x08

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
bool hal_input_shouldExit();   // ESC or BACKSPACE pressed?
```

### Audio (`hal_audio.h`)
```cpp
void hal_audio_init();         // Setup LEDC timer on piezo pin
void hal_audio_play(uint16_t freq, uint32_t duration_ms);
void hal_audio_stop();
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
void hal_rtc_getDateTime(hal_rtc_datetime_t* dt);
uint32_t hal_rtc_getUnixTime();
```

### IMU (`hal_imu.h`)
```cpp
bool hal_imu_init();                // Returns false (no IMU on Mini)
bool hal_imu_getAccel(float* x, float* y, float* z);  // Stub: x=0, y=0, z=1.0
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
| KBD `BACKSPACE` | Btn B (GPIO 34) |

### Display layout (320×170)
```
TOP_BAR     = 18px   (status + notifications)
MAIN_AREA   = 134px  (mode content)
BOTTOM_BAR  = 18px   (bottom overlay)
```

## Convention

- **Commits**: One commit per meaningful phase (no batching)
- **Branch**: `esp32-s3-mini-port` for this port — `main` is upstream M5Cardputer
- **Style**: Arduino framework, C++17, snake_case for functions, PascalCase for classes
