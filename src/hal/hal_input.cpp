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
static bool stableState[5] = {true, true, true, true, true}; // pull-up → HIGH = released
static bool lastRaw[5] = {true, true, true, true, true};
static uint32_t lastDebounce[5] = {0, 0, 0, 0, 0};
static const uint32_t DEBOUNCE_MS = 50;

// Edge detection
static bool risingEdge[5] = {false, false, false, false, false};
static bool consumed[5] = {false, false, false, false, false};

// Change flag
static bool stateChanged = false;

void hal_input_init() {
    for (int i = 0; i < NUM_JOY_PINS; i++) {
        pinMode(JOY_PINS[i], INPUT_PULLUP);
        stableState[i] = true;
        lastRaw[i] = true;
        consumed[i] = true; // don't fire on initial reading
    }
}

void hal_gpio_setup() {
    // Joystick inputs (with internal pull-ups)
    hal_input_init();
    
    // Piezo buzzer as output
    pinMode(PIN_PIEZO, OUTPUT);
    digitalWrite(PIN_PIEZO, LOW);
    
    // NeoPixel output — handled by hal_neopixel_init, but ensure pin is set
    pinMode(PIN_NEOPIXEL, OUTPUT);
    digitalWrite(PIN_NEOPIXEL, LOW);
    
    // Display backlight — handled by hal_display_init
    pinMode(PIN_DISPLAY_BL, OUTPUT);
    digitalWrite(PIN_DISPLAY_BL, LOW);
}

void hal_input_update() {
    uint32_t now = millis();
    stateChanged = false;

    for (int i = 0; i < NUM_JOY_PINS; i++) {
        bool raw = (digitalRead(JOY_PINS[i]) == LOW); // LOW = pressed

        if (raw != lastRaw[i]) {
            lastDebounce[i] = now;
            lastRaw[i] = raw;
        }

        if ((now - lastDebounce[i]) >= DEBOUNCE_MS && raw != stableState[i]) {
            // Stable transition
            stableState[i] = raw;
            
            // Only capture rising edges (presses, not releases)
            if (raw) {
                risingEdge[i] = true;
                consumed[i] = false;
                stateChanged = true;
            }
        }
    }
}

uint8_t hal_input_getch() {
    for (int i = 0; i < NUM_JOY_PINS; i++) {
        if (risingEdge[i] && !consumed[i]) {
            consumed[i] = true;
            return JOY_KEYS[i];
        }
    }
    return 0;
}

bool hal_input_wasPressed(uint8_t keyCode) {
    for (int i = 0; i < NUM_JOY_PINS; i++) {
        if (JOY_KEYS[i] == keyCode && risingEdge[i] && !consumed[i]) {
            consumed[i] = true;
            return true;
        }
    }
    return false;
}

bool hal_input_anyHeld() {
    for (int i = 0; i < NUM_JOY_PINS; i++) {
        if (stableState[i]) return true; // any pressed
    }
    return false;
}

void hal_input_waitRelease() {
    while (hal_input_anyHeld()) {
        hal_input_update();
        delay(10);
        yield();
    }
}

bool hal_input_isChange() {
    return stateChanged;
}

InputEvent hal_input_keysState() {
    InputEvent ev = {false, 0};
    if (!digitalRead(JOYSTICK_UP_PIN)) { ev.pressed = true; ev.key = KEY_UP; return ev; }
    if (!digitalRead(JOYSTICK_DOWN_PIN)) { ev.pressed = true; ev.key = KEY_DOWN; return ev; }
    if (!digitalRead(JOYSTICK_LEFT_PIN)) { ev.pressed = true; ev.key = KEY_LEFT; return ev; }
    if (!digitalRead(JOYSTICK_RIGHT_PIN)) { ev.pressed = true; ev.key = KEY_RIGHT; return ev; }
    if (!digitalRead(JOYSTICK_CENTER_PIN)) { ev.pressed = true; ev.key = KEY_ENTER; return ev; }
    return ev;
}
bool hal_input_isKeyPressed(char key) {
    switch (key) {
        case KEY_UP: return !digitalRead(JOYSTICK_UP_PIN);
        case KEY_DOWN: return !digitalRead(JOYSTICK_DOWN_PIN);
        case KEY_LEFT: return !digitalRead(JOYSTICK_LEFT_PIN);
        case KEY_RIGHT: return !digitalRead(JOYSTICK_RIGHT_PIN);
        case KEY_ENTER: return !digitalRead(JOYSTICK_CENTER_PIN);
        default: return false;
    }
}
bool hal_input_shouldExit() {
    return hal_input_wasPressed(KEY_ESC) || hal_input_wasPressed(KEY_BACKSPACE);
}
bool hal_input_isPressed() {
    return hal_input_anyHeld();
}