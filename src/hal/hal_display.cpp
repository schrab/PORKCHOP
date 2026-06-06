#include "hal_display.h"
#include "hal_pins.h"
#include <cstdio>

TFT_eSPI g_Display = TFT_eSPI();

// ============================================================
// DisplayCanvas — DisplayCanvas-compatible wrapper using TFT_eSprite
// ============================================================

DisplayCanvas::DisplayCanvas(TFT_eSPI* display)
    : m_display(display)
    , m_sprite(nullptr)
{
}

DisplayCanvas::~DisplayCanvas() {
    deleteSprite();
}

void DisplayCanvas::createSprite(int32_t w, int32_t h) {
    deleteSprite(); // clean up previous if any
    m_sprite = new TFT_eSprite(m_display);
    m_sprite->setColorDepth(8);
    m_sprite->createSprite(w, h);
}

void DisplayCanvas::deleteSprite() {
    if (m_sprite) {
        m_sprite->deleteSprite();
        delete m_sprite;
        m_sprite = nullptr;
    }
}

void DisplayCanvas::setColorDepth(int8_t b) {
    if (m_sprite) m_sprite->setColorDepth(b);
}

void DisplayCanvas::fillSprite(uint32_t color) {
    if (m_sprite) m_sprite->fillSprite(color);
}

void DisplayCanvas::fillScreen(uint32_t color) {
    m_display->fillScreen(color);
}

void DisplayCanvas::pushSprite(int32_t x, int32_t y) {
    if (m_sprite) m_sprite->pushSprite(x, y);
}

void DisplayCanvas::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (m_sprite) m_sprite->fillRect(x, y, w, h, color);
}

void DisplayCanvas::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (m_sprite) m_sprite->drawRect(x, y, w, h, color);
}

void DisplayCanvas::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color) {
    if (m_sprite) m_sprite->fillRoundRect(x, y, w, h, r, color);
}

void DisplayCanvas::fillCircle(int32_t x, int32_t y, int32_t r, uint32_t color) {
    if (m_sprite) m_sprite->fillCircle(x, y, r, color);
}

void DisplayCanvas::fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color) {
    if (m_sprite) m_sprite->fillTriangle(x0, y0, x1, y1, x2, y2, color);
}

void DisplayCanvas::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
    if (m_sprite) m_sprite->drawLine(x0, y0, x1, y1, color);
}

void DisplayCanvas::drawPixel(int32_t x, int32_t y, uint32_t color) {
    if (m_sprite) m_sprite->drawPixel(x, y, color);
}

void DisplayCanvas::drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t color) {
    if (m_sprite) m_sprite->drawFastHLine(x, y, w, color);
}

void DisplayCanvas::drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t color) {
    if (m_sprite) m_sprite->drawFastVLine(x, y, h, color);
}

void DisplayCanvas::setTextColor(uint16_t fg, uint16_t bg) {
    if (m_sprite) m_sprite->setTextColor(fg, bg);
}

void DisplayCanvas::setTextSize(float size) {
    if (m_sprite) m_sprite->setTextSize(size);
}

void DisplayCanvas::setTextDatum(uint8_t datum) {
    if (m_sprite) m_sprite->setTextDatum(datum);
}

void DisplayCanvas::setFont(const void* font) {
    if (m_sprite) m_sprite->setFreeFont((const GFXfont*)font);
}

void DisplayCanvas::setCursor(int32_t x, int32_t y) {
    if (m_sprite) m_sprite->setCursor(x, y);
}

void DisplayCanvas::drawString(const char* str, int32_t x, int32_t y) {
    if (m_sprite) m_sprite->drawString(str, x, y);
}

void DisplayCanvas::drawCentreString(const char* str, int32_t x, int32_t y) {
    if (m_sprite) m_sprite->drawCentreString(str, x, y, 1);
}

void DisplayCanvas::drawRightString(const char* str, int32_t x, int32_t y) {
    if (m_sprite) m_sprite->drawRightString(str, x, y, 1);
}

uint16_t DisplayCanvas::color565(uint8_t r, uint8_t g, uint8_t b) {
    return m_display->color565(r, g, b);
}

uint16_t DisplayCanvas::color24to16(uint32_t c) {
    return m_display->color24to16(c);
}

// ============================================================
// Display init helper
// ============================================================
void hal_display_init() {
    g_Display.init();
    g_Display.setRotation(1); // landscape: 320x170
    g_Display.fillScreen(TFT_BLACK);
    g_Display.setTextColor(TFT_WHITE, TFT_BLACK);
    // Backlight on
    pinMode(PIN_DISPLAY_BL, OUTPUT);
    digitalWrite(PIN_DISPLAY_BL, HIGH);
}

// Additional methods for M5Canvas compatibility
void DisplayCanvas::setTextColor(uint16_t fg) {
    if (m_sprite) m_sprite->setTextColor(fg);
}

void DisplayCanvas::print(const char* str) {
    if (m_sprite && str) m_sprite->print(str);
}

void DisplayCanvas::print(int val) {
    if (m_sprite) m_sprite->print(val);
}

void DisplayCanvas::print(float val) {
    if (m_sprite) m_sprite->print(val);
}

void DisplayCanvas::drawChar(char c, int32_t x, int32_t y) {
    if (m_sprite) {
        m_sprite->setCursor(x, y);
        m_sprite->print(c);
    }
}

int16_t DisplayCanvas::width() const {
    return m_sprite ? m_sprite->width() : 0;
}

int16_t DisplayCanvas::height() const {
    return m_sprite ? m_sprite->height() : 0;
}

int16_t DisplayCanvas::textWidth(const char* str) const {
    if (!m_sprite || !str) return 0;
    return m_sprite->textWidth(str);
}

// Set display brightness (0-255)
void hal_display_setBrightness(uint8_t brightness) {
    ledcAttachPin(PIN_DISPLAY_BL, DISPLAY_BL_LEDC_CHANNEL);
    ledcWrite(DISPLAY_BL_LEDC_CHANNEL, brightness);
}