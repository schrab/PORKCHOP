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
static bool stableState[5] = {false, false, false, false, false}; // pull-up → HIGH = released
static bool lastRaw[5] = {false, false, false, false, false};
static uint32_t lastDebounce[5] = {0, 0, 0, 0, 0};
static const uint32_t DEBOUNCE_MS = 50;

// Edge detection
static bool risingEdge[5] = {false, false, false, false, false};

// Long-press LEFT → ESC synthesis
static uint32_t leftPressStart = 0;
static bool leftLongFired = false;
static const uint32_t LONG_PRESS_MS = 800;

// Long-press ENTER → action synthesis
static uint32_t enterPressStart = 0;
static bool enterLongFired = false;

// Long-press RIGHT → filter cycle (SPECTRUM)
static uint32_t rightPressStart = 0;
static bool rightLongFired = false;
static bool rightLongConsumed = false;  // stays true until release, prevents re-arm

// Long-press UP → attack mode (SPECTRUM)
static uint32_t upPressStart = 0;
static bool upLongFired = false;
static bool upLongConsumed = false;

// Change flag
static bool stateChanged = false;

void hal_input_init() {
    for (int i = 0; i < NUM_JOY_PINS; i++) {
        pinMode(JOY_PINS[i], INPUT_PULLUP);
        stableState[i] = false;
        lastRaw[i] = false;
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
                stateChanged = true;
            }
        }
    }

    // Long-press LEFT (index 2) → synthesize KEY_ESC
    bool leftHeld = stableState[2];
    if (leftHeld) {
        if (leftPressStart == 0) {
            leftPressStart = now;
            leftLongFired = false;
        } else if (!leftLongFired && (now - leftPressStart) >= LONG_PRESS_MS) {
            leftLongFired = true;
            stateChanged = true;
        }
    } else {
        leftPressStart = 0;
        leftLongFired = false;
    }

    // Long-press ENTER (index 4)
    bool enterHeld = stableState[4];
    if (enterHeld) {
        if (enterPressStart == 0) {
            enterPressStart = now;
            enterLongFired = false;
        } else if (!enterLongFired && (now - enterPressStart) >= LONG_PRESS_MS) {
            enterLongFired = true;
            stateChanged = true;
        }
    } else {
        enterPressStart = 0;
        enterLongFired = false;
    }

    // Long-press RIGHT (index 3) → filter cycle
    bool rightHeld = stableState[3];
    if (rightHeld) {
        if (rightPressStart == 0) {
            rightPressStart = now;
            rightLongFired = false;
            rightLongConsumed = false;
        } else if (!rightLongFired && !rightLongConsumed && (now - rightPressStart) >= LONG_PRESS_MS) {
            rightLongFired = true;
            rightLongConsumed = true;
            stateChanged = true;
        }
    } else {
        rightPressStart = 0;
        rightLongFired = false;
        rightLongConsumed = false;
    }

    // Long-press UP (index 0) → attack mode (SPECTRUM)
    bool upHeld = stableState[0];
    if (upHeld) {
        if (upPressStart == 0) {
            upPressStart = now;
            upLongFired = false;
            upLongConsumed = false;
        } else if (!upLongFired && !upLongConsumed && (now - upPressStart) >= LONG_PRESS_MS) {
            upLongFired = true;
            upLongConsumed = true;
            stateChanged = true;
        }
    } else {
        upPressStart = 0;
        upLongFired = false;
        upLongConsumed = false;
    }
}

uint8_t hal_input_getch() {
    for (int i = 0; i < NUM_JOY_PINS; i++) {
        if (risingEdge[i]) {
            risingEdge[i] = false;
            return JOY_KEYS[i];
        }
    }
    return 0;
}

bool hal_input_wasPressed(uint8_t keyCode) {
    // Check synthesized ESC from long-press LEFT
    if (keyCode == KEY_ESC && leftLongFired) {
        leftLongFired = false;
        return true;
    }
    for (int i = 0; i < NUM_JOY_PINS; i++) {
        if (JOY_KEYS[i] == keyCode && risingEdge[i]) {
            risingEdge[i] = false;
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
    return hal_input_wasPressed(KEY_ESC);
}
bool hal_input_isPressed() {
    return hal_input_anyHeld();
}

bool hal_input_isLongEnter() {
    if (enterLongFired) {
        enterLongFired = false;
        return true;
    }
    return false;
}

bool hal_input_isLongRight() {
    if (rightLongFired) {
        rightLongFired = false;
        return true;
    }
    return false;
}

bool hal_input_isLongUp() {
    if (upLongFired) {
        upLongFired = false;
        return true;
    }
    return false;
}

void hal_input_consumeLongEsc() {
    leftLongFired = false;
}