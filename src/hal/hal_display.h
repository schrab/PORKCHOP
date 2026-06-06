#pragma once
#include <cstdint>
#include <TFT_eSPI.h>

// M5GFX text datum constants — values match TFT_eSPI natively
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

// Thin wrapper around TFT_eSprite to match DisplayCanvas API used by PORKCHOP
class DisplayCanvas {
public:
    DisplayCanvas(TFT_eSPI* display);
    ~DisplayCanvas();

    // Lifecycle
    void createSprite(int32_t w, int32_t h);
    void deleteSprite();
    void setColorDepth(int8_t b);

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
    void setTextColor(uint16_t fg, uint16_t bg);
    void setTextSize(float size);
    void setTextDatum(uint8_t datum);
    void setFont(const void* font);
    void setCursor(int32_t x, int32_t y);
    void drawString(const char* str, int32_t x, int32_t y);
    void drawCentreString(const char* str, int32_t x, int32_t y);
    void drawRightString(const char* str, int32_t x, int32_t y);

    // Color conversion
    static uint16_t color565(uint8_t r, uint8_t g, uint8_t b);
    static uint16_t color24to16(uint32_t c);

    // Raw access for callers that need it
    TFT_eSprite* getSprite() { return m_sprite; }

private:
    TFT_eSPI* m_display;
    TFT_eSprite* m_sprite;
};

// Global display driver (extern, defined in hal_display.cpp)
extern TFT_eSPI g_Display;
