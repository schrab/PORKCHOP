1|#pragma once
2|
3|#include <Arduino.h>
4|5|#include "../core/sd_format.h"
6|
7|class SdFormatMenu {
8|public:
9|    static void show();
10|    static void hide();
11|    static void update();
12|    static bool isActive() { return active; }
13|    static void draw(DisplayCanvas& canvas);
14|    static const char* getSelectedDescription();
15|    
16|    // Bar-less mode: SD format runs without top/bottom bars to save RAM
17|    static bool areBarsHidden() { return barsHidden; }
18|
19|private:
20|    enum class State : uint8_t {
21|        CONFIRM_ENTRY,  // Warning dialog before entering (Y/N)
22|        SELECT,         // Format mode selection (QUICK/FULL)
23|        CONFIRM,        // Final confirmation before format
24|        WORKING,        // Formatting in progress
25|        RESULT          // Format complete, waiting for reboot
26|    };
27|
28|    static bool active;
29|    static bool keyWasPressed;
30|    static State state;
31|    static SDFormat::Result lastResult;
32|    static SDFormat::FormatMode formatMode;
33|    static uint8_t progressPercent;
34|    static char progressStage[32];  // Increased from 16 for ETA strings like "ERASE ~1h23m"
35|    
36|    // System state
37|    static bool barsHidden;      // True when bars are hidden (saves RAM)
38|    static bool systemStopped;   // True when NetworkRecon/WiFi stopped
39|
40|    // Hint system
41|    static const char* const HINTS[];
42|    static const uint8_t HINT_COUNT;
43|    static uint8_t hintIndex;
44|
45|    static void handleInput();
46|    static void startFormat();
47|    static void stopEverything();     // Stop NetworkRecon, FileServer, WiFi
48|    static void doReboot();           // Reboot with countdown
49|    static void drawConfirmEntry(DisplayCanvas& canvas);
50|    static void drawSelect(DisplayCanvas& canvas);
51|    static void drawConfirm(DisplayCanvas& canvas);
52|    static void drawWorking(DisplayCanvas& canvas);
53|    static void drawResult(DisplayCanvas& canvas);
54|    static void onFormatProgress(const char* stage, uint8_t percent);
55|};
56|