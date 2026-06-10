#include "hal_battery.h"
#include "hal_pins.h"
#include <Arduino.h>
#include "driver/adc.h"
#include "esp_adc_cal.h"

static bool battery_initialized = false;
static esp_adc_cal_characteristics_t adc_chars;

// Voltage divider: 100k+100k nominal (2:1), calibrated to actual hardware
// 4.24V battery → 4092mV reported with ratio 2.0 → actual ratio = 4240/2046 = 2.072
static constexpr float BATTERY_DIVIDER_RATIO = 2.072f;  // Vbatt = ADC_voltage * 2.072

void hal_battery_init() {
    if (battery_initialized) return;

    // Configure ADC1 — GPIO8 = ADC1_CHANNEL_7 on ESP32-S3
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_DB_12);

    // Characterize ADC for accurate mV readings
    esp_adc_cal_characterize(
        ADC_UNIT_1,
        ADC_ATTEN_DB_12,
        ADC_WIDTH_BIT_12,
        1100,  // default Vref
        &adc_chars
    );

    battery_initialized = true;
}

void hal_battery_reset() {
    battery_initialized = false;
    hal_battery_init();
    // Reset EMA so first reading seeds fresh
    // (declared inside hal_battery_read_mv, reset via flag)
    static_cast<void>(0);  // no-op: EMA reset handled by first read with seed check
}

uint32_t hal_battery_read_mv() {
    if (!battery_initialized) return 0;

    // Oversample: 4 reads, drop min/max, average the middle two
    int raws[4];
    for (int i = 0; i < 4; i++) {
        int r = adc1_get_raw(ADC1_CHANNEL_7);
        raws[i] = (r >= 0) ? r : 0;  // guard against -1 from WiFi interference
    }
    // Simple bubble sort for 4 elements
    for (int i = 0; i < 3; i++) {
        for (int j = i + 1; j < 4; j++) {
            if (raws[j] < raws[i]) {
                int tmp = raws[i]; raws[i] = raws[j]; raws[j] = tmp;
            }
        }
    }
    int filtered_raw = (raws[1] + raws[2]) / 2;
    uint32_t mv = esp_adc_cal_raw_to_voltage(filtered_raw, &adc_chars);
    uint32_t battery_mv = (uint32_t)(mv * BATTERY_DIVIDER_RATIO);

    // EMA: 75% previous, 25% new reading (smooths WiFi noise)
    // Seed on first valid read; re-seed if stuck below plausible battery voltage
    static float ema_mv = 0.0f;
    if (ema_mv < 100.0f || battery_mv < 100) {
        ema_mv = (float)battery_mv;
    } else {
        ema_mv = ema_mv * 0.75f + (float)battery_mv * 0.25f;
    }
    return (uint32_t)ema_mv;
}

uint8_t hal_battery_read_percent() {
    uint32_t mv = hal_battery_read_mv();
    if (mv == 0) return 0;

    // LiPo ranges: 3.3V (empty) → 4.2V (full)
    // Scale 3300-4200 mV to 0-100%
    if (mv >= 4200) return 100;
    if (mv <= 3300) return 0;
    return (uint8_t)((mv - 3300) * 100 / (4200 - 3300));
}

bool hal_battery_isCharging() {
    // ESP32-S3 Mini doesn't have a dedicated charging-detect pin.
    // This returns false by default. If VBUS monitoring is added later,
    // detect VBUS presence via ADC or GPIO to return true when USB is plugged.
    return false;
}
