1|// Charging Mode - Low power battery display
2|#pragma once
3|
4|#include <Arduino.h>
5|6|
7|class ChargingMode {
8|public:
9|    static void start();
10|    static void stop();
11|    static void update();
12|    static void draw(M5Canvas& canvas);
13|    
14|    static bool isRunning() { return running; }
15|    static bool shouldExit() { return exitRequested; }
16|    static void clearExit() { exitRequested = false; }
17|    static bool areBarsHidden() { return barsHidden; }
18|    
19|    // Battery info getters for display
20|    static uint8_t getBatteryPercent() { return batteryPercent; }
21|    static float getBatteryVoltage() { return batteryVoltage; }
22|    static bool isCharging() { return charging; }
23|    static int getMinutesToFull() { return minutesToFull; }
24|    
25|private:
26|    static bool running;
27|    static bool exitRequested;
28|    static bool keyWasPressed;
29|    static bool barsHidden;
30|    
31|    // Battery state
32|    static uint8_t batteryPercent;
33|    static float batteryVoltage;
34|    static bool charging;
35|    static int minutesToFull;
36|    
37|    // Voltage tracking for charge rate estimation
38|    static float voltageHistory[10];
39|    static uint8_t voltageHistoryIdx;
40|    static uint32_t lastVoltageMs;
41|    static uint32_t lastUpdateMs;
42|    
43|    // Animation
44|    static uint8_t animFrame;
45|    static uint32_t lastAnimMs;
46|
47|    // Exit/unplug detection
48|    static uint32_t unplugDetectMs;
49|
50|    // Charge-rate estimate tracking
51|    static float lastEstimateVoltage;
52|    static uint32_t lastEstimateMs;
53|
54|    // Session state snapshots
55|    static bool reconWasActive;
56|    static bool gpsWasActive;
57|    static bool wifiWasOn;
58|
59|    // External power tracking
60|    static bool powerPresent;
61|    static bool powerSeen;
62|    static uint32_t lastChargingMs;
63|    static float entryVoltage;
64|    static float peakVoltage;
65|    static bool trendPowerPresent;
66|    
67|    // Calculate battery percentage from voltage (more accurate than AXP)
68|    static uint8_t voltageToPercent(float voltage, bool isCharging);
69|    
70|    // Estimate minutes to full based on charge rate
71|    static int estimateMinutesToFull();
72|    
73|    static void handleInput();
74|    static void updateBattery();
75|};
76|