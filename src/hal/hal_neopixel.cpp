#include "hal_neopixel.h"
#include "hal_pins.h"
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

// Note: Adafruit NeoPixel library must be added to lib_deps in platformio.ini
// We'll add it there.
static Adafruit_NeoPixel* neopixel = nullptr;

void hal_neopixel_init() {
    if (neopixel) return;
    neopixel = new Adafruit_NeoPixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
    neopixel->begin();
    neopixel->setBrightness(NEOPIXEL_POWER);
    neopixel->show(); // off initially
}

void hal_neopixel_set(uint8_t r, uint8_t g, uint8_t b) {
    if (!neopixel) return;
    neopixel->setPixelColor(0, neopixel->Color(r, g, b));
    neopixel->show();
}

void hal_neopixel_off() {
    if (!neopixel) return;
    neopixel->setPixelColor(0, 0);
    neopixel->show();
}

// Convert HSV hue (0-360) to RGB and set
void hal_neopixel_set_hue(uint16_t hue) {
    if (!neopixel) return;
    uint16_t h = hue % 360;
    uint8_t r, g, b;

    // Simplified HSV → RGB for single LED
    uint8_t region = h / 60;
    uint8_t remainder = (h % 60) * 255 / 60;

    switch (region) {
        case 0:  r = 255; g = remainder;       b = 0;          break;
        case 1:  r = 255 - remainder; g = 255; b = 0;          break;
        case 2:  r = 0;             g = 255; b = remainder;    break;
        case 3:  r = 0;             g = 255 - remainder; b = 255; break;
        case 4:  r = remainder;     g = 0;          b = 255;    break;
        default: r = 255;           g = 0;          b = 255 - remainder; break;
    }

    hal_neopixel_set(r, g, b);
}
