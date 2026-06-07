# Display Port Report: PORKCHOP → ESP32-S3 Mini

## Problem

Porting PORKCHOP firmware from M5Cardputer (240×135 ST7789) to custom ESP32-S3 Mini board (320×170 ST7789). The display is the primary output device and must show pixel graphics, text, UI elements correctly.

## Hardware

| Property | Value |
|----------|-------|
| Controller | ST7789 |
| Resolution | 320 × 170 (landscape) |
| Native GRAM | 240 × 320 (portrait) |
| Interface | 4-wire SPI |
| SPI Host | SPI3_HOST (HSPI) |
| SPI Frequency | 20 MHz (tried 40 MHz) |
| Pins | MOSI=14, SCLK=15, CS=11, DC=12, RST=13, BL=10 |
| Library | TFT_eSPI v2.5.43 |
| Framework | PlatformIO + Arduino ESP32 |

## Display Addressing

The ST7789 has a 240 × 320 GRAM. Our display shows a 320 × 170 window. To achieve this:
- **MV bit** (MADCTL bit 5) swaps row/column addressing, mapping GRAM rows to display columns and GRAM columns to display rows
- With MV=1: display X = GRAM rows (0-319, 320px), display Y = GRAM columns (start..start+169, 170px)
- A **Y-offset** of 35 shifts the visible window to GRAM columns 35-204 (170 columns)

## Approaches Tried

### 1. SPI Port Selection (TFT_eSPI `USE_HSPI_PORT`)

| Attempt | Config | Result |
|---------|--------|--------|
| No `USE_HSPI_PORT` | Default `SPI` (SPI2_HOST/FSPI) | StoreProhibited crash — wrong SPI host, pins not mapped |
| `USE_HSPI_PORT` defined | `SPIClass(HSPI)` = SPI3_HOST | Board boots, display works |

**Winner**: `USE_HSPI_PORT` is required. Creates a dedicated `SPIClass(HSPI)` instance for SPI3_HOST.

### 2. Display Dimensions (`TFT_WIDTH`, `TFT_HEIGHT`)

| Attempt | Config | Result |
|---------|--------|--------|
| 320×170, no CGRAM_OFFSET | Portrait: 320×170, rotation swaps → _width=170, _height=320 | Left 2/3 of screen used |
| 170×320, no CGRAM_OFFSET | Portrait: 170×320, rotation swaps → _width=320, _height=170 | Full width, correct |
| 170×320, CGRAM_OFFSET | Adds rowstart=35 via CGRAM code | Full width + row offset |

**Winner**: `TFT_WIDTH=170, TFT_HEIGHT=320` with `CGRAM_OFFSET` for correct addressing.

### 3. CGRAM Offset (`CGRAM_OFFSET`)

The ST7789_Rotation.h has conditional CGRAM offset code for common resolutions:

| `_init_width` | Rotation 1 (colstart, rowstart) | Rotation 3 (colstart, rowstart) |
|---|---|---|
| 170 | 0, 35 | 0, 35 |
| Other | 0, 0 | 80, 0 |

| Attempt | Config | Result |
|---------|--------|--------|
| No CGRAM_OFFSET | colstart=0, rowstart=0 | Content uses full 170 rows, no offset |
| CGRAM_OFFSET | colstart=0, rowstart=35 | Content shifted up 35px, bottom 35px black/static |

**Issue**: With CGRAM_OFFSET, the 35px rowstart shifts content up but creates unwritten GRAM area at bottom (static or black depending on GRAM contents).

### 4. Rotation / MADCTL

TFT_eSPI sends MADCTL during `setRotation()`:

| Rotation | MADCTL | Meaning | User report |
|----------|--------|---------|-------------|
| 0 | 0x28 (MV\|BGR) | Portrait, no swap | N/A |
| 1 | 0x68 (MX\|MV\|BGR) | Landscape, X flipped | Same as all others |
| 2 | 0xA8 (MY\|MV\|BGR) | Portrait inverted | N/A |
| 3 | 0xE8 (MX\|MY\|MV\|BGR) | Landscape, both flipped | Same as all others |

| Attempt | Result |
|---------|--------|
| Rotation 1 (MX\|MV\|BGR=0x68) | "180° rotated" |
| Rotation 3 (MV\|MY\|BGR=0xA8) | "180° rotated" |
| Manual MADCTL=0x08 (MV\|BGR, no MX/MY) | "180° rotated" |
| Manual MADCTL=0xE8 (MX\|MY\|MV\|BGR) | "180° rotated" |
| Manual MADCTL=0x00 (no bits) | "180° rotated" |
| Manual MADCTL=0x20 (MV only) | "180° rotated" |

**Conclusion**: The MX and MY bits have **no effect** on this display module. Only the MV bit works (swap). The display is **always 180° rotated** relative to expected orientation regardless of MADCTL settings.

### 5. Manual CASET/RASET Override

Sent commands 0x2A (CASET) and 0x2B (RASET) directly after `setRotation()`:

| Attempt | CASET | RASET | Result |
|---------|-------|-------|--------|
| CGRAM_OFFSET (via TFT_eSPI) | 0-319 | 35-204 | Correct width, but overridden on every draw call |
| Manual override | 0-319 | 35-204 | Only affects first fillScreen, subsequent draws reset |
| Manual with `startWrite` | 0-319 | 35-204 | Same — TFT_eSPI sets window per-draw |

**Issue**: Manual CASET/RASET is only effective until the next TFT_eSPI drawing call, which calls `setAddrWindow()` and resets the window.

### 6. Sprite Usage (mainCanvas)

| Attempt | Result |
|---------|--------|
| mainCanvas sprite (320×142, 8-bit, PSRAM) | Double-buffering eliminates flicker, but buffer gets corrupted → RGB static on right 1/3, bottom noise |
| No sprites, draw directly to `g_Display` | No static, but UI freezes at splash screen (missing sprite operations in display.cpp) |
| mainCanvas sprite with fallback | Sprite created if possible, fallback to g_Display if allocation fails |

**Issue**: The 45KB sprite buffer gets corrupted between `Display::init()` and first `Display::update()` call. Pointer `m_sprite` value becomes `0xA5A5A5A5` (freed heap memory fill pattern). Root cause not found but may be unrelated heap corruption from NVS/Preferences initialization.

### 7. SPI Frequency

| Attempt | Frequency | Result |
|---------|-----------|--------|
| Default TFT_eSPI | 20 MHz | Works, stable |
| WiFiTool match | 40 MHz | Works, stable |

**Conclusion**: Both 20 MHz and 40 MHz work. WiFiTool uses 40 MHz.

### 8. Reference Project Comparison (ESP32S3_WiFiTool)

The WiFiTool project uses `esp_lcd` (ESP-IDF LCD framework) + LVGL, not TFT_eSPI.

| Setting | WiFiTool | PORKCHOP (TFT_eSPI equivalent) |
|---------|----------|-------------------------------|
| `swap_xy(true)` | MV=1 | Part of rotation 1/3 |
| `mirror(false, true)` | MX=0, MY=1 | Rotation 3 (MV\|MY\|BGR) |
| `set_gap(0, 35)` | X-gap=0, Y-gap=35 | CGRAM_OFFSET → rowstart=35 |
| MADCTL | 0x08 (MV\|BGR) | Can't match — MV bit needed for swap |
| Resolution | 320×170 | 170×320 (portrait, swapped by rotation) |
| SPI speed | 40 MHz | 20-40 MHz |
| Color order | BGR | `TFT_RGB_ORDER TFT_BGR` |
| Inversion | Enabled | `TFT_INVERSION_ON` |

**Note**: WiFiTool's MADCTL=0x08 sends only MV+BGR without MX/MY because it sends MADCTL DURING init (before rotation). TFT_eSPI's init also sends MADCTL=0x08 (via ST7789_Init.h line 19: `writedata(TFT_MAD_COLOR_ORDER)` which is 0x08 when BGR). Then `setRotation()` overrides with rotation-specific MADCTL. The display might be locking to the FIRST MADCTL received.

## Current State

### Working
- ✅ Display boots and shows content
- ✅ Full 320×170 resolution
- ✅ Colors correct (pink on black = default theme)
- ✅ UI loads and runs — no crash
- ✅ All three sprite canvases (topBar, mainCanvas, bottomBar) functional
- ✅ Input works
- ✅ SPI stable at 40 MHz on SPI3_HOST (HSPI)
- ✅ PSRAM active (8MB quad, qio_qspi SDK variant)
- ✅ Sprite corruption crash **fixed** — see fix_attempts_report.md for full root cause

### Not Working
- ❌ **180° rotation** — MX/MY MADCTL bits have no effect on this display module. Content is upside-down. Fix requires software pixel reversal in TFT_eSPI.
- ❌ **Dim wake** — after dimming, keys sometimes don't restore brightness.

## Remaining Work

1. **Fix 180° rotation**: Patch TFT_eSPI's `pushSprite()` / `setAddrWindow()` to reverse
   row order in software, or flip the UI coordinate system in `display.cpp`.

2. **Fix dimming wake**: Investigate LEDC PWM restore path in `hal_display_setBrightness()`.

## Key Files

| File | Purpose |
|------|---------|
| `src/User_Setup.h` | TFT_eSPI pin/display config |
| `src/hal/hal_display.cpp` | Display init, DisplayCanvas wrapper |
| `src/hal/hal_display.h` | DisplayCanvas class definition |
| `src/ui/display.cpp` | UI rendering (topBar, mainCanvas, bottomBar) |
| `src/ui/display.h` | Display layout constants (DISPLAY_W, DISPLAY_H) |
| `platformio.ini` | [env:esp32s3-mini] build config |
| `sdkconfig.esp32s3_mini` | ESP-IDF Kconfig for PSRAM, SPI, etc. |

## Quick Config Reference

```cpp
// Working TFT_eSPI config (User_Setup.h)
#define USER_SETUP_LOADED
#define ST7789_DRIVER
#define CGRAM_OFFSET
#define TFT_WIDTH  170
#define TFT_HEIGHT 320
#define USE_HSPI_PORT
#define TFT_MOSI  14
#define TFT_SCLK  15
#define TFT_CS    11
#define TFT_DC    12
#define TFT_RST   13
#define TFT_BL    10
// g_Display.setRotation(3);  // in hal_display_init()
```
