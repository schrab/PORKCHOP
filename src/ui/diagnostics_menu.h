1|// Diagnostics Menu - System status snapshot
2|#pragma once
3|
4|#include <Arduino.h>
5|6|
7|class DiagnosticsMenu {
8|public:
9|    static void show();
10|    static void hide();
11|    static void update();
12|    static bool isActive() { return active; }
13|    static void draw(M5Canvas& canvas);
14|
15|private:
16|    static bool active;
17|    static bool keyWasPressed;
18|    static uint16_t cachedWpaCracked;
19|    static uint16_t cachedWigleUploaded;
20|    static uint32_t lastStatRefreshMs;
21|    static uint32_t statRefreshIntervalMs;
22|    static void saveSnapshot();
23|    static void resetWiFi();
24|    static void logHeapSnapshot();
25|    static void collectGarbage();
26|    static void refreshStats();
27|};
28|