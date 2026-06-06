#pragma once

class DisplayCanvas;

#include <Arduino.h>
#include "../core/sd_format.h"

class SdFormatMenu {
public:
    static void show();
    static void hide();
    static void update();
    static bool isActive() { return active; }
    static void draw(DisplayCanvas& canvas);
    static const char* getSelectedDescription();
    
    // Bar-less mode: SD format runs without top/bottom bars to save RAM
    static bool areBarsHidden() { return barsHidden; }

private:
    enum class State : uint8_t {
        CONFIRM_ENTRY,  // Warning dialog before entering (Y/N)
        SELECT,         // Format mode selection (QUICK/FULL)
        CONFIRM,        // Final confirmation before format
        WORKING,        // Formatting in progress
        RESULT          // Format complete, waiting for reboot
    };

    static bool active;
    static bool keyWasPressed;
    static State state;
    static SDFormat::Result lastResult;
    static SDFormat::FormatMode formatMode;
    static uint8_t progressPercent;
    static char progressStage[32];  // Increased from 16 for ETA strings like "ERASE ~1h23m"
    
    // System state
    static bool barsHidden;      // True when bars are hidden (saves RAM)
    static bool systemStopped;   // True when NetworkRecon/WiFi stopped

    // Hint system
    static const char* const HINTS[];
    static const uint8_t HINT_COUNT;
    static uint8_t hintIndex;

    static void handleInput();
    static void startFormat();
    static void stopEverything();     // Stop NetworkRecon, FileServer, WiFi
    static void doReboot();           // Reboot with countdown
    static void drawConfirmEntry(DisplayCanvas& canvas);
    static void drawSelect(DisplayCanvas& canvas);
    static void drawConfirm(DisplayCanvas& canvas);
    static void drawWorking(DisplayCanvas& canvas);
    static void drawResult(DisplayCanvas& canvas);
    static void onFormatProgress(const char* stage, uint8_t percent);
};
