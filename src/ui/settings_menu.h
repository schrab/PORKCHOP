1|// Settings menu system
2|#pragma once
3|
4|#include <Arduino.h>
5|6|
7|enum class SettingType {
8|    TOGGLE,     // ON/OFF
9|    VALUE,      // Numeric value with min/max
10|    ACTION,     // Trigger action (like Save)
11|    TEXT        // Text input (SSID, password, etc.)
12|};
13|
14|class SettingsMenu {
15|public:
16|    static void init();
17|    static void update();
18|    static void draw(DisplayCanvas& canvas);
19|    
20|    static void show();
21|    static void hide();
22|    static bool isActive() { return active; }
23|    static bool shouldExit() { return exitRequested; }
24|    static void clearExit() { exitRequested = false; }
25|    static const char* getSelectedDescription();
26|    
27|private:
28|    static bool active;
29|    static bool exitRequested;
30|    static bool keyWasPressed;
31|    static bool editing;  // Currently adjusting a value
32|    static bool textEditing;  // Currently editing text
33|    static char textBuffer[80];   // Buffer for text input (max field is 64 chars)
34|    static uint8_t textLen;
35|    static uint8_t rootIndex;
36|    static uint8_t rootScroll;
37|    static uint8_t groupIndex;
38|    static uint8_t groupScroll;
39|    static uint8_t activeGroup;
40|    static uint8_t textEditId;
41|    static uint32_t lastInputMs;
42|    static bool dirtyConfig;
43|    static bool dirtyPersonality;
44|    static uint8_t origGpsRxPin;
45|    static uint8_t origGpsTxPin;
46|    static uint32_t origGpsBaud;
47|    static uint8_t origGpsSource;
48|
49|    static const uint8_t VISIBLE_ROOT_ITEMS = 5;
50|    static const uint8_t VISIBLE_GROUP_ITEMS = 4;
51|    static const uint32_t AUTO_SAVE_MS = 3000;
52|
53|    static void handleInput();
54|    static void handleTextInput();
55|    static void maybeAutoSave();
56|    static void saveIfDirty(bool showToast);
57|};
58|