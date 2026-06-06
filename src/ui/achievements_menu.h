1|// Achievements Menu - View unlocked achievements
2|#pragma once
3|
4|#include <Arduino.h>
5|6|
7|class AchievementsMenu {
8|public:
9|    static void init();
10|    static void show();
11|    static void hide();
12|    static void update();
13|    static void draw(DisplayCanvas& canvas);
14|    static bool isActive() { return active; }
15|    static const uint8_t TOTAL_ACHIEVEMENTS = 63;  // 48 base + 12 DNH/BOAR + 3 CLIENT MONITOR
16|    
17|private:
18|    static uint8_t selectedIndex;
19|    static uint8_t scrollOffset;
20|    static bool active;
21|    static bool keyWasPressed;
22|    static bool showingDetail;  // Showing achievement detail popup
23|    
24|    static const uint8_t VISIBLE_ITEMS = 5;
25|    
26|    static void handleInput();
27|    static void drawDetail(DisplayCanvas& canvas);
28|    static void updateBottomOverlay();
29|};
30|