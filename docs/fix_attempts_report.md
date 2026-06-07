# Display Port — What Was Done and Why

## Overview
Porting PORKCHOP from M5Cardputer (240×135) to ESP32-S3 Mini (320×170 ST7789) using TFT_eSPI.

---

## ✅ Resolved Issues

### PSRAM Configuration (Board + SDK)
- **Root cause**: `board = esp32-s3-devkitc-1` doesn't have PSRAM.
- **Fix**: Created `boards/esp32s3_mini_n16r8_quad.json` with `memory_type: "qio_qspi"`. Added `board_build.psram = enable`, `board_build.psram_mode = quad` in platformio.ini.
- **Status**: PSRAM initializes silently. "opi psram: wrong PSRAM line mode" error gone.

### SPI Host (USE_HSPI_PORT)
- Display is on SPI3_HOST (HSPI). TFT_eSPI needs `USE_HSPI_PORT`.
- Without it: StoreProhibited crash at begin_tft_write.

### Display Dimensions + CGRAM Offset
- TFT_WIDTH=170, TFT_HEIGHT=320 (portrait native). Rotation 3 swaps to 320×170.
- CGRAM_OFFSET with `_init_width=170` gives rowstart=35.

### Colors
- `TFT_RGB_ORDER TFT_BGR` + `TFT_INVERSION_ON`.

### Large char buffers moved to PSRAM (.ext_ram.bss)
- `Display::toastMessage[160]`, `topBarMessage[96]`, `bottomOverlay[96]`,
  `pendingTopBarMessageBuf[96]`, `uploadStatus[64]`, `lootSSID[20]`
- Tagged with `__attribute__((section(".ext_ram.bss")))` in display.cpp.
- Moves them from internal SRAM BSS (adjacent to DisplayCanvas objects) into PSRAM.
- **Note**: `.ext_ram.bss` is the correct section name — the framework's `sections.ld` already
  routes it to PSRAM. The `boards/custom.ld` / `boards/esp32s3_out.ld` fragment using `> ext_ram`
  does NOT work because `ext_ram` is not a defined MEMORY region in the linker scope available
  to secondary `-T` scripts. Using `.ext_ram.bss` directly requires no linker script change.

### Display::init() rotation conflict removed
- `Display::init()` previously called `g_Display.setRotation(1)`, overriding the rotation 3
  already set by `hal_display_init()`. Removed that call.

### Sprite corruption: TFT_eSprite embedded by value (root cause fixed)
See detailed analysis below.

---

## Root Cause Analysis — 0xA5A5A5A5 Crash

### Symptom
Every boot: `Guru Meditation Error: Core 1 panic'ed (LoadProhibited)` at
`TFT_eSPI::textWidth()` called from `Display::drawBottomBar()`. Register A9 = `0xa5a5a5a5`,
EXCVADDR = `0xa5a5a5ad` (A9 + 8).

Backtrace confirmed via addr2line:
```
TFT_eSPI::textWidth()
TFT_eSPI::drawString()
DisplayCanvas::drawString()       ← hal_display.cpp:38
Display::drawBottomBar()
Display::update()
loop()
```

### What 0xA5A5A5A5 means
ESP-IDF heap poisoning (`CONFIG_HEAP_POISONING_COMPREHENSIVE`) fills freed heap blocks with
`0xa5` immediately on `free()`/`delete`. Reading `0xa5a5a5a5` from a pointer means the
object at that address was heap-allocated, then freed, and the memory is now poisoned.

### Why `m_sprite` became 0xA5A5A5A5

`DisplayCanvas` stored a `TFT_eSprite*` pointer (`m_sprite`) allocated with `new`. The
`TFT_eSprite` object itself lived on the **internal SRAM heap** at some address X.

The firmware does a large number of heap alloc/free cycles during boot:
- `setupHeapLayout()` allocates an 80KB fence, inits WiFi, frees the fence
- `Config::init()` mounts SD, SPIFFS, reads files
- `XP::load()` calls `Preferences::begin()` → NVS open (fails, but still touches heap)
- `NetworkRecon::start()` potentially calls `NimBLEDevice::deinit(true)` later

ESP-IDF's TLSF allocator reuses freed blocks in address order. After enough alloc/free
cycles, the heap block at address X — which `bottomBar.m_sprite` pointed to — got reused
by another allocation, then freed and poisoned with `0xa5`. But `m_sprite` still held
address X. The next read through `m_sprite` hit poisoned memory.

**The key insight**: `TFT_eSprite* m_sprite = new TFT_eSprite(...)` puts the *object itself*
(~600 bytes of TFT_eSprite member variables) on the heap. Any heap churn that frees that
block corrupts the pointer. This is fundamentally different from the sprite's *pixel buffer*
(which is in PSRAM via `ps_calloc` — that was never the problem).

### Why moving char buffers to .ext_ram.bss didn't fix it
The char buffers (`toastMessage` etc.) were adjacent to `bottomBar` in BSS and could have
been a buffer overflow source, but the actual corruption mechanism was heap reuse, not
BSS overflow. Moving them to PSRAM eliminated the overflow risk but didn't prevent the
TLSF reuse of the `TFT_eSprite` heap block.

### The Fix
**Embed `TFT_eSprite` by value inside `DisplayCanvas`** instead of allocating via `new`.

```cpp
// Before (broken):
class DisplayCanvas {
    TFT_eSprite* m_sprite;   // heap pointer — vulnerable to TLSF reuse + poison
};

// After (fixed):
class DisplayCanvas {
    bool         m_created;  // pixel buffer allocated?
    mutable TFT_eSprite m_sprite;  // embedded by value — lives in BSS, never freed
};
```

The `TFT_eSprite` object now lives in **BSS** alongside `m_display` and `m_created`.
BSS is static storage — it's never passed to `free()`, never poisoned, never reused.
The only heap allocation remaining is the sprite's **pixel buffer** (via `ps_calloc` into
PSRAM), which is managed correctly by `TFT_eSprite::createSprite()` /
`TFT_eSprite::deleteSprite()`. A `bool m_created` flag replaces the null-pointer guard.

---

## Remaining Issues

### ❌ Display rotated 180°
- MX and MY bits in MADCTL have no effect on this display module.
- All tested MADCTL values (0x08, 0x68, 0xA8, 0xE8, 0x00, 0x20) produce the same orientation.
- **Cannot be fixed via register writes.** Must fix in software (flip pixel order before SPI
  transfer) or rotate UI coordinate system.

### ❌ Display shifted / CGRAM offset
- `CGRAM_OFFSET` is NOT defined in User_Setup.h (deliberately omitted after offset analysis).
- Display currently shows correctly without offset. If offset issues appear, revisit.

### ❌ Dim wake
- After screen dims, key presses sometimes don't restore brightness.
- Not investigated.

---

## Current Working Config

### User_Setup.h
```cpp
#define USER_SETUP_LOADED
#define ST7789_DRIVER
#define TFT_WIDTH  170
#define TFT_HEIGHT 320
#define USE_HSPI_PORT
#define TFT_MOSI  14  #define TFT_SCLK  15  #define TFT_CS    11
#define TFT_DC    12  #define TFT_RST   13  #define TFT_BL    10
#define SPI_FREQUENCY  40000000
#define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_ON
```

### hal_display_init() rotation
```cpp
g_Display.setRotation(3);   // MV|MY|BGR — DO NOT override in Display::init()
```

### DisplayCanvas (hal_display.h / hal_display.cpp)
- `TFT_eSprite` embedded by value, not pointer
- `bool m_created` guards all sprite operations
- `createSprite()` only allocates pixel buffer (no `new`/`delete` of object)

### Large buffers in PSRAM
```cpp
// display.cpp static members:
char Display::toastMessage[160]           __attribute__((section(".ext_ram.bss")));
char Display::topBarMessage[96]           __attribute__((section(".ext_ram.bss")));
char Display::bottomOverlay[96]           __attribute__((section(".ext_ram.bss")));
char Display::pendingTopBarMessageBuf[96] __attribute__((section(".ext_ram.bss")));
char Display::uploadStatus[64]            __attribute__((section(".ext_ram.bss")));
static char lootSSID[20]                  __attribute__((section(".ext_ram.bss")));
```

---

## Key Files

| File | Purpose |
|------|---------|
| `boards/esp32s3_mini_n16r8_quad.json` | Custom board: qio_qspi, PSRAM enabled |
| `boards/esp32s3_out.ld` | Linker fragment (`.psram_bss` section — currently unused, `.ext_ram.bss` used instead) |
| `boards/custom.ld` | Identical to esp32s3_out.ld — not wired into build |
| `platformio.ini` | [env:esp32s3-mini] build config |
| `src/User_Setup.h` | TFT_eSPI pin/display config |
| `src/hal/hal_display.cpp` | DisplayCanvas impl — embedded TFT_eSprite, no new/delete |
| `src/hal/hal_display.h` | DisplayCanvas class — TFT_eSprite by value |
| `src/ui/display.cpp` | Three-canvas sprite system, large buffers in .ext_ram.bss |
| `docs/display_port_report.md` | Full display technical report |
