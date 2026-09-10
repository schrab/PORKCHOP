#include "hal_display.h"
#include "hal_pins.h"
#include <cstdio>
#include <SPI.h>
#include "driver/ledc.h"

TFT_eSPI g_Display = TFT_eSPI();

// TFT_eSprite requires a TFT_eSPI* at construction time.
// We pass &g_Display so all three canvases share the same driver instance.
DisplayCanvas::DisplayCanvas(TFT_eSPI* display)
    : m_display(display), m_created(false), m_sprite(display) {}

DisplayCanvas::~DisplayCanvas() { deleteSprite(); }

void DisplayCanvas::createSprite(int32_t w, int32_t h, int8_t bpp) {
    // Release existing pixel buffer (does not destroy the object)
    if (m_created) {
        m_sprite.deleteSprite();
        m_created = false;
    }
    // Force internal RAM for sprite buffer to avoid PSRAM bus stall TG1WDT
    // when Core 0 is busy with WiFi handshake processing (OINK capture).
    // Must happen BEFORE createSprite, not after setColorDepth re-creates it.
    m_sprite.setAttribute(PSRAM_ENABLE, 0);
    m_sprite.setColorDepth(bpp);
    void* buf = m_sprite.createSprite(w, h);
    m_created = (buf != nullptr);
    Serial.printf("[DISPLAY] createSprite(%dx%d, %dbpp) -> buf=%p (created=%d) free_int=%u largest_int=%u\r\n",
                  (int)w, (int)h, (int)bpp, buf, (int)m_created,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void DisplayCanvas::deleteSprite() {
    if (m_created) {
        m_sprite.deleteSprite();
        m_created = false;
    }
}

void DisplayCanvas::setColorDepth(int8_t b) { if (m_created) m_sprite.setColorDepth(b); }
void DisplayCanvas::setAttribute(int16_t attr, int16_t val) { m_sprite.setAttribute(attr, val); }
void DisplayCanvas::fillSprite(uint32_t color) { if (m_created) m_sprite.fillSprite(color); else if (m_display) m_display->fillScreen(color); }
void DisplayCanvas::fillScreen(uint32_t color) { if (m_display) m_display->fillScreen(color); }
void DisplayCanvas::pushSprite(int32_t x, int32_t y) { if (m_created) m_sprite.pushSprite(x, y); }
void DisplayCanvas::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) { if (m_created) m_sprite.fillRect(x, y, w, h, c); else if (m_display) m_display->fillRect(x, y, w, h, c); }
void DisplayCanvas::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) { if (m_created) m_sprite.drawRect(x, y, w, h, c); else if (m_display) m_display->drawRect(x, y, w, h, c); }
void DisplayCanvas::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t c) { if (m_created) m_sprite.fillRoundRect(x, y, w, h, r, c); }
void DisplayCanvas::fillCircle(int32_t x, int32_t y, int32_t r, uint32_t c) { if (m_created) m_sprite.fillCircle(x, y, r, c); }
void DisplayCanvas::fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t c) { if (m_created) m_sprite.fillTriangle(x0, y0, x1, y1, x2, y2, c); }
void DisplayCanvas::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) { if (m_created) m_sprite.drawLine(x0, y0, x1, y1, c); }
void DisplayCanvas::drawPixel(int32_t x, int32_t y, uint32_t c) { if (m_created) m_sprite.drawPixel(x, y, c); }
void DisplayCanvas::drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t c) { if (m_created) m_sprite.drawFastHLine(x, y, w, c); }
void DisplayCanvas::drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t c) { if (m_created) m_sprite.drawFastVLine(x, y, h, c); }
void DisplayCanvas::setTextColor(uint16_t fg, uint16_t bg) { if (m_created) m_sprite.setTextColor(fg, bg); }
void DisplayCanvas::setTextColor(uint16_t fg) { if (m_created) m_sprite.setTextColor(fg); }
void DisplayCanvas::setTextSize(float s) { if (m_created) m_sprite.setTextSize(s); }
void DisplayCanvas::setTextDatum(uint8_t d) { if (m_created) m_sprite.setTextDatum(d); }
void DisplayCanvas::setFont(const void* f) { if (m_created) m_sprite.setFreeFont((const GFXfont*)f); }
void DisplayCanvas::setCursor(int32_t x, int32_t y) { if (m_created) m_sprite.setCursor(x, y); else if (m_display) m_display->setCursor(x, y); }
void DisplayCanvas::drawString(const char* s, int32_t x, int32_t y) { if (m_created) m_sprite.drawString(s, x, y); }
void DisplayCanvas::drawCentreString(const char* s, int32_t x, int32_t y) { if (m_created) m_sprite.drawCentreString(s, x, y, 1); }
void DisplayCanvas::drawRightString(const char* s, int32_t x, int32_t y) { if (m_created) m_sprite.drawRightString(s, x, y, 1); }
uint16_t DisplayCanvas::color565(uint8_t r, uint8_t g, uint8_t b) { return m_display->color565(r, g, b); }
uint16_t DisplayCanvas::color24to16(uint32_t c) { return m_display->color24to16(c); }
void DisplayCanvas::print(const char* s) { if (m_created && s) m_sprite.print(s); }
void DisplayCanvas::print(int v) { if (m_created) m_sprite.print(v); }
void DisplayCanvas::print(float v) { if (m_created) m_sprite.print(v); }
void DisplayCanvas::drawChar(char c, int32_t x, int32_t y) { if (m_created) { m_sprite.setCursor(x, y); m_sprite.print(c); } }
int16_t DisplayCanvas::width() const { return m_created ? m_sprite.width() : (m_display ? m_display->width() : 0); }
int16_t DisplayCanvas::height() const { return m_created ? m_sprite.height() : (m_display ? m_display->height() : 0); }
int16_t DisplayCanvas::textWidth(const char* s) const { if (!m_created || !s) return 0; return m_sprite.textWidth(s); }

// Track whether backlight LEDC has been initialized
static bool bl_ledc_initialized = false;

void hal_display_init() {
    pinMode(PIN_DISPLAY_RST, OUTPUT);
    digitalWrite(PIN_DISPLAY_RST, LOW);
    delay(10);
    digitalWrite(PIN_DISPLAY_RST, HIGH);
    delay(120);

    g_Display.init();
    g_Display.setAttribute(PSRAM_ENABLE, 1);
    g_Display.setRotation(3);
    g_Display.fillScreen(TFT_BLACK);
    g_Display.setTextColor(TFT_WHITE, TFT_BLACK);
    pinMode(PIN_DISPLAY_BL, OUTPUT);
    digitalWrite(PIN_DISPLAY_BL, HIGH);
}

void hal_display_setBrightness(uint8_t brightness) {
    if (!bl_ledc_initialized) {
        // One-time LEDC setup using ESP-IDF API (avoids Arduino/ESP-IDF API conflicts)
        // Uses separate timer (TIMER_1) and channel (CHANNEL_1) from piezo (TIMER_0/CHANNEL_0)
        ledc_timer_config_t bl_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .timer_num = DISPLAY_BL_LEDC_TIMER,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK
        };
        ledc_timer_config(&bl_timer);

        ledc_channel_config_t bl_channel = {
            .gpio_num = PIN_DISPLAY_BL,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = DISPLAY_BL_LEDC_CHANNEL,
            .timer_sel = DISPLAY_BL_LEDC_TIMER,
            .duty = brightness,
            .hpoint = 0
        };
        ledc_channel_config(&bl_channel);
        bl_ledc_initialized = true;
    } else {
        // Just update duty cycle
        ledc_set_duty(LEDC_LOW_SPEED_MODE, DISPLAY_BL_LEDC_CHANNEL, brightness);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, DISPLAY_BL_LEDC_CHANNEL);
    }
}
