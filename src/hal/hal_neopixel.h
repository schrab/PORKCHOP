#pragma once
#include <cstdint>

void hal_neopixel_init();
void hal_neopixel_set(uint8_t r, uint8_t g, uint8_t b);
void hal_neopixel_off();
void hal_neopixel_set_hue(uint16_t hue); // 0-360
