// User_Setup.h — TFT_eSPI configuration for ESP32-S3 Mini
// ST7789, 320x170 landscape, HSPI (SPI3_HOST), custom pins
// Config derived from ESP32S3_WiFiTool project
#define USER_SETUP_LOADED
// Driver
#define ST7789_DRIVER

// Display dimensions (landscape)
#define TFT_WIDTH  320
#define TFT_HEIGHT 170

// SPI pins — display on SPI3_HOST (HSPI)
#define TFT_MOSI  14
#define TFT_SCLK  15
#define TFT_CS    11
#define TFT_DC    12
#define TFT_RST   13
#define TFT_BL    10

// SPI frequency
#define SPI_FREQUENCY  20000000
#define SPI_READ_FREQUENCY  10000000
#define SPI_TOUCH_FREQUENCY  2500000

// Display configuration from WiFiTool: BGR order, inversion on
#define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_ON

// Backlight control
#define TFT_BACKLIGHT_ON HIGH

// Font support
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// DMA not available
// #define USE_DMA
