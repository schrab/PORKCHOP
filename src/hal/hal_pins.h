#pragma once
#include <cstdint>
#include "driver/spi_common.h"

// ============================================================
// ESP32-S3 Mini Pin Definitions
// Reference: WiFiTool board_config.cpp configureESP32S3Mini()
// ============================================================

// --- Display: ST7789 via SPI3_HOST ---
#define PIN_DISPLAY_MOSI      14
#define PIN_DISPLAY_SCLK      15
#define PIN_DISPLAY_CS        11
#define PIN_DISPLAY_DC        12
#define PIN_DISPLAY_RST       13
#define PIN_DISPLAY_BL        10
#define PIN_DISPLAY_MISO      -1   // unused
#define DISPLAY_WIDTH         320
#define DISPLAY_HEIGHT        170
#define DISPLAY_SPI_HOST      SPI3_HOST

// --- SD Card: via SPI2_HOST ---
#define PIN_SD_MOSI           5
#define PIN_SD_MISO           3
#define PIN_SD_SCLK           4
#define PIN_SD_CS             6
#define SD_SPI_HOST           SPI2_HOST

// --- 5-Way Joystick (digital inputs, active low with pull-up) ---
#define PIN_JOY_UP            40
#define PIN_JOY_DOWN          39
#define PIN_JOY_LEFT          41
#define PIN_JOY_RIGHT         42
#define PIN_JOY_SELECT        38

// --- GPS: UART ---
#define PIN_GPS_TX            1
#define PIN_GPS_RX            2
#define GPS_UART_NUM          1     // UART1
#define GPS_BAUD_DEFAULT      9600

// --- NeoPixel ---
#define PIN_NEOPIXEL          33
#define NEOPIXEL_POWER        200   // default brightness 0-255

// --- Battery ADC ---
#define PIN_BATTERY_ADC       8
#define BATTERY_ADC_ATTEN     ADC_ATTEN_DB_12
#define BATTERY_ADC_WIDTH     ADC_WIDTH_BIT_12

// --- Piezo Buzzer (passive, ledc PWM) ---
#define PIN_PIEZO             7
#define PIEZO_LEDC_TIMER      LEDC_TIMER_0
#define PIEZO_LEDC_CHANNEL    LEDC_CHANNEL_0
#define DISPLAY_BL_LEDC_CHANNEL    LEDC_CHANNEL_2
#define PIEZO_LEDC_RESOLUTION LEDC_TIMER_8_BIT
#define DISPLAY_BL_LEDC_CHANNEL    0

// --- Add missing aliases for HAL compatibility ---
#define JOYSTICK_UP_PIN       PIN_JOY_UP
#define JOYSTICK_DOWN_PIN     PIN_JOY_DOWN
#define JOYSTICK_LEFT_PIN     PIN_JOY_LEFT
#define JOYSTICK_RIGHT_PIN    PIN_JOY_RIGHT
#define JOYSTICK_CENTER_PIN   PIN_JOY_SELECT
#define BTN_A_PIN             33  // TODO: verify
#define BTN_B_PIN             34  // TODO: verify
#define SPEAKER_PIN           PIN_PIEZO
