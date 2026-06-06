#pragma once
#include <cstdint>

// Battery voltage reading via ADC
void hal_battery_init();
// Returns voltage in millivolts (mV), or 0 if not available
uint32_t hal_battery_read_mv();
// Returns approximated percentage (0-100)
uint8_t hal_battery_read_percent();
bool hal_battery_isCharging();

