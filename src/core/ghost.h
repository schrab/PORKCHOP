#pragma once

#include <Arduino.h>
#include <esp_mac.h>

namespace GhostMode {

void init();
void apply();     // apply current MAC (random or real)
void update();
bool isActive();
void getMAC(uint8_t* out);
void realMAC(uint8_t* out);

} // namespace GhostMode
