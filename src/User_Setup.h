// User_Setup.h — TFT_eSPI configuration for ESP32-S3 Mini
// ST7789, 320x170 landscape, SPI3_HOST, custom pins
// This file overrides TFT_eSPI's default setup via -include in build_flags

#define USER_SETUP_INFO "ESP32-S3 Mini ST7789 320x170"

// Driver
#define ST7789_DRIVER

// Display dimensions (landscape)
#define TFT_WIDTH  320
#define TFT_HEIGHT 170

// SPI pins (SPI3_HOST)
#define TFT_MOSI  14
#define TFT_SCLK  15
#define TFT_CS    11
#define TFT_DC    12
#define TFT_RST   13
#define TFT_BL    10

// Optional MISO pin (unused for display)
// #define TFT_MISO  -1

// SPI frequency
#define SPI_FREQUENCY  40000000   // 40MHz
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000

// Backlight control
#define TFT_BACKLIGHT_ON HIGH

// Font support — load all fonts
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

// Smooth font
#define SMOOTH_FONT

// SPI bus selection — use VSPI (SPI3_HOST on ESP32-S3)
// TFT_eSPI maps SPI bus via these defines
#define TFT_SPI_PORT 3   // SPI3_HOST

// Colour inversion for some ST7789 modules
// #define TFT_INVERSION_ON

// DMA not available on this configuration
// #define USE_DMA
