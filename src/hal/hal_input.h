#pragma once
#include <cstdint>

// Key event codes — same as PORKCHOP's M5Cardputer.Keyboard codes
#define KEY_UP      0xDA
#define KEY_DOWN    0xD9
#define KEY_LEFT    0xD8
#define KEY_RIGHT   0xD7
#define KEY_ENTER   0x0D
#define KEY_ESC     0x1B
#define KEY_BACKSPACE 0x08
#define KEY_TAB     0x09

// Input event structure
struct InputEvent {
    bool pressed;
    uint8_t key;
};

// Initialize joystick GPIOs
void hal_input_init();

// Poll and debounce current state — call once per main loop iteration
void hal_input_update();

// Return the last-read key if it changed state since last update, 0 otherwise
// After reading, the key change is consumed (won't be returned again)
uint8_t hal_input_getch();

// Check if a specific key was just pressed (clears the pressed flag)
bool hal_input_wasPressed(uint8_t keyCode);

// Check if any key is currently held
bool hal_input_anyHeld();

// Wait until all keys are released (with watchdog yield)
void hal_input_waitRelease();

// Check if any key state changed since last update
bool hal_input_isChange();
