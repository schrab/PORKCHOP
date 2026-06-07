// User_Setup.h — ESP32-S3 Mini final attempt
#define USER_SETUP_LOADED
#define ST7789_DRIVER

// Portrait 170x320 — rotation 1/3 swap to landscape
#define TFT_WIDTH  170
#define TFT_HEIGHT 320

#define USE_HSPI_PORT
#define TFT_MOSI  14
#define TFT_SCLK  15
#define TFT_CS    11
#define TFT_DC    12
#define TFT_RST   13
#define TFT_BL    10

// 40MHz SPI — matches WiFiTool
#define SPI_FREQUENCY  40000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000

#define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_ON

#define TFT_BACKLIGHT_ON HIGH
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
