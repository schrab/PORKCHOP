1|// BOAR BROS Menu - Manage excluded networks
2|#pragma once
3|
4|#include <Arduino.h>
5|6|#include <vector>
7|
8|struct BroInfo {
9|    uint64_t bssid;      // BSSID as uint64
10|    char bssidStr[18];   // Formatted BSSID (AA:BB:CC:DD:EE:FF)
11|    char ssid[33];       // SSID if known (from file comment)
12|};
13|
14|class BoarBrosMenu {
15|public:
16|    static void init();
17|    static void show();
18|    static void hide();
19|    static void update();
20|    static void draw(M5Canvas& canvas);
21|    static bool isActive() { return active; }
22|    static size_t getCount();
23|    static void getSelectedInfo(char* out, size_t len);
24|    
25|private:
26|    static std::vector<BroInfo> bros;
27|    static uint8_t selectedIndex;
28|    static uint8_t scrollOffset;
29|    static bool active;
30|    static bool keyWasPressed;
31|    static bool deleteConfirmActive;
32|    
33|    static const uint8_t VISIBLE_ITEMS = 5;
34|    
35|    static void handleInput();
36|    static void loadBros();
37|    static void deleteSelected();
38|    static void drawDeleteConfirm(M5Canvas& canvas);
39|    static void formatBSSID(uint64_t bssid, char* out, size_t len);
40|};
41|