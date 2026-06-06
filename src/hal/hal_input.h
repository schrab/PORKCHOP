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

// Read current state, return event if something changed
InputEvent hal_input_read();

// Debounce helpers
bool hal_input_wasPressed(uint8_t key);
