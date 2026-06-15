#pragma once
#include <cstdint>
#include <TFT_eSPI.h>

// DisplayCanvas text datum constants — values match TFT_eSPI natively
enum m5_textdatum_t {
    top_left       = 0,
    top_center     = 1,
    top_right      = 2,
    middle_left    = 3,
    middle_center  = 4,
    middle_right   = 5,
    bottom_left    = 6,
    bottom_center  = 7,
    bottom_right   = 8,
};

// Thin wrapper around TFT_eSprite to match DisplayCanvas API used by PORKCHOP.
//
// IMPORTANT: TFT_eSprite is embedded by VALUE (not pointer) to avoid heap
// allocation.  Storing TFT_eSprite* on the heap and then new/delete-ing it
// made the pointer vulnerable to TLSF heap-poison overwrites (0xa5a5a5a5)
// from adjacent BLE/WiFi deallocation, causing LoadProhibited crashes.
// Embedding the object in BSS alongside m_display makes it immune.
class DisplayCanvas {
public:
    explicit DisplayCanvas(TFT_eSPI* display);
    ~DisplayCanvas();

    // Lifecycle — PSRAM pixel buffer allocation only (object itself is embedded)
    void createSprite(int32_t w, int32_t h, int8_t bpp = 16);
    void deleteSprite();
    void setColorDepth(int8_t b);
    void setAttribute(int16_t attr, int16_t val);

    // Drawing
    void fillSprite(uint32_t color);
    void fillScreen(uint32_t color);
    void pushSprite(int32_t x, int32_t y);
    void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
    void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
    void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color);
    void fillCircle(int32_t x, int32_t y, int32_t r, uint32_t color);
    void fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color);
    void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);
    void drawPixel(int32_t x, int32_t y, uint32_t color);
    void drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t color);
    void drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t color);

    // Text
    void setTextColor(uint16_t fg);
    void setTextColor(uint16_t fg, uint16_t bg);
    void setTextSize(float size);
    void setTextDatum(uint8_t datum);
    void setFont(const void* font);
    void setCursor(int32_t x, int32_t y);
    void drawString(const char* str, int32_t x, int32_t y);
    void drawCentreString(const char* str, int32_t x, int32_t y);
    void drawRightString(const char* str, int32_t x, int32_t y);
    void print(const char* str);
    void print(int val);
    void print(float val);
    void drawChar(char c, int32_t x, int32_t y);

    // Canvas dimensions
    int16_t width() const;
    int16_t height() const;
    int16_t textWidth(const char* str) const;

    // Color conversion
    uint16_t color565(uint8_t r, uint8_t g, uint8_t b);
    uint16_t color24to16(uint32_t c);

    // Raw access — returns pointer to embedded sprite (never null after construction)
    TFT_eSprite* getSprite() { return m_created ? &m_sprite : nullptr; }

private:
    TFT_eSPI*           m_display;   // pointer to the shared TFT driver (4 bytes)
    bool                m_created;   // true once createSprite() has succeeded
    mutable TFT_eSprite m_sprite;    // embedded by value — lives in BSS, never heap-allocated
};

// Global display driver (extern, defined in hal_display.cpp)
extern TFT_eSPI g_Display;

// Initialize display subsystem
void hal_display_init();
void hal_display_setBrightness(uint8_t brightness);

// Global display driver (extern, defined in hal_display.cpp)
extern TFT_eSPI g_Display;

// Initialize display subsystem
void hal_display_init();
void hal_display_setBrightness(uint8_t brightness);
