1|// Crash Viewer Menu
2|#pragma once
3|
4|#include <Arduino.h>
5|#include <vector>
6|7|
8|class CrashViewer {
9|public:
10|    static void init();
11|    static void show();
12|    static void hide();
13|    static void update();
14|    static bool isActive() { return active; }
15|    static void draw(DisplayCanvas& canvas);
16|    static void getStatusLine(char* out, size_t len);
17|
18|    struct LogLine { char text[80]; };
19|
20|private:
21|    struct CrashEntry {
22|        char path[64];
23|        time_t timestamp = 0;
24|    };
25|    static bool active;
26|    static std::vector<CrashEntry> crashFiles;
27|    static std::vector<LogLine> fileLines;
28|    static uint16_t listScroll;
29|    static uint16_t fileScroll;
30|    static uint16_t totalLines;
31|    static uint8_t selectedIndex;
32|    static bool fileViewActive;
33|    static bool nukeConfirmActive;
34|    static bool keyWasPressed;
35|    static char activeFile[64];
36|
37|    static void scanCrashFiles();
38|    static void loadCrashFile(const char* path);
39|    static void drawList(DisplayCanvas& canvas);
40|    static void drawFile(DisplayCanvas& canvas);
41|    static void drawNukeConfirm(DisplayCanvas& canvas);
42|    static void nukeCrashFiles();
43|};
44|