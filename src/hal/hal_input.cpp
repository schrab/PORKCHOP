#include "hal_input.h"
#include "hal_pins.h"
#include <Arduino.h>

static const int JOY_PINS[] = {
    PIN_JOY_UP, PIN_JOY_DOWN, PIN_JOY_LEFT, PIN_JOY_RIGHT, PIN_JOY_SELECT
};
static const uint8_t JOY_KEYS[] = {
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER
};
static const int NUM_JOY_PINS = sizeof(JOY_PINS) / sizeof(JOY_PINS[0]);

// Debounce state
static bool lastState[5] = {true, true, true, true, true}; // pull-up → HIGH = released
static uint32_t lastDebounce[5] = {0, 0, 0, 0, 0};
static const uint32_t DEBOUNCE_MS = 50;

// Pressed key tracking (for hold detection)
static uint8_t currentKey = 0;
static bool currentPressed = false;
static uint32_t keyPressStart = 0;
static bool keyHandled = false;

void hal_input_init() {
    for (int i = 0; i < NUM_JOY_PINS; i++) {
        pinMode(JOY_PINS[i], INPUT_PULLUP);
        lastState[i] = true; // HIGH = not pressed
    }
}

InputEvent hal_input_read() {
    InputEvent ev = {false, 0};
    uint32_t now = millis();

    for (int i = 0; i < NUM_JOY_PINS; i++) {
        bool reading = digitalRead(JOY_PINS[i]) == LOW; // LOW = pressed (pull-up)

        // Debounce
        if (reading != lastState[i]) {
            lastDebounce[i] = now;
            lastState[i] = reading;
        }

        if ((now - lastDebounce[i]) >= DEBOUNCE_MS && reading) {
            ev.pressed = true;
            ev.key = JOY_KEYS[i];
            return ev;
        }
    }

    return ev;
}

bool hal_input_wasPressed(uint8_t key) {
    (void)key;
    // For long-press detection: called from the main loop to check
    // if a key is held beyond the repeat threshold.
    // Returns true once per press event.
    InputEvent ev = hal_input_read();
    if (ev.pressed) {
        return true;
    }
    return false;
}
