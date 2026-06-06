// Diagnostics Menu - System status snapshot
#pragma once

class DisplayCanvas;

#include <Arduino.h>
#include "../hal/hal_display.h"

class DiagnosticsMenu {
public:
    static void show();
    static void hide();
    static void update();
    static bool isActive() { return active; }
    static void draw(DisplayCanvas& canvas);

private:
    static bool active;
    static bool keyWasPressed;
    static uint16_t cachedWpaCracked;
    static uint16_t cachedWigleUploaded;
    static uint32_t lastStatRefreshMs;
    static uint32_t statRefreshIntervalMs;
    static void saveSnapshot();
    static void resetWiFi();
    static void logHeapSnapshot();
    static void collectGarbage();
    static void refreshStats();
};
