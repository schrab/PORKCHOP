1|// Bounty Status Menu - View bounties to send to kid (Sirloin)
2|// Porkchop sends wardriven networks to Sirloin for hunting
3|#pragma once
4|
5|#include <Arduino.h>
6|7|
8|class BountyStatusMenu {
9|public:
10|    static void init();
11|    static void show();
12|    static void hide();
13|    static void update();
14|    static void draw(M5Canvas& canvas);
15|    static bool isActive() { return active; }
16|    static void getSelectedInfo(char* out, size_t len);
17|    
18|private:
19|    static uint16_t selectedIndex;   // uint16_t: supports up to 65535 bounties (MAX_SEEN_BSSIDS=5000)
20|    static uint16_t scrollOffset;    // uint16_t: matches selectedIndex
21|    static bool active;
22|    static bool keyWasPressed;
23|    
24|    // Layout constants (match boar_bros_menu pattern - no header)
25|    static const uint8_t VISIBLE_ITEMS = 6;  // 6 items, full canvas
26|    static const int LINE_H = 17;            // Line height (107px / 6 items)
27|    static const int COL_LEFT = 4;           // Left margin
28|    
29|    static void handleInput();
30|    static void drawList(M5Canvas& canvas);
31|    static void drawEmpty(M5Canvas& canvas);
32|};
33|