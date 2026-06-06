1|// Piglet mood and phrases
2|#pragma once
3|
4|5|#include "avatar.h"
6|
7|class Mood {
8|public:
9|    static void init();
10|    static void update();
11|    static void draw(DisplayCanvas& canvas);
12|    static void saveMood();  // Phase 10: Save mood to NVS
13|    
14|    // Mood triggers
15|    static void onHandshakeCaptured(const char* apName = nullptr);
16|    static void onPMKIDCaptured(const char* apName = nullptr);
17|    static void onNewNetwork(const char* apName = nullptr, int8_t rssi = 0, uint8_t channel = 0);
18|    static void setStatusMessage(const char* msg);  // For mode-specific info
19|    static void onMLPrediction(float confidence);
20|    static void onNoActivity(uint32_t seconds);
21|    static void onWiFiLost();
22|    static void onGPSFix();
23|    static void onGPSLost();
24|    static void onLowBattery();
25|    
26|    // Context-aware mood updates
27|    static void onSniffing(uint16_t networkCount, uint8_t channel);
28|    static void onPassiveRecon(uint16_t networkCount, uint8_t channel);  // DO NO HAM mode
29|    static void onDeauthing(const char* apName, uint32_t deauthCount);
30|    static void onDeauthSuccess(const uint8_t* clientMac);  // Client disconnected!
31|    static void onBored(uint16_t networkCount = 0);  // No valid targets available
32|    static void onIdle();
33|    static void onWarhogUpdate();
34|    static void onWarhogFound(const char* apName = nullptr, uint8_t channel = 0);
35|    static void onPiggyBluesUpdate(const char* vendor = nullptr, int8_t rssi = 0, uint8_t targetCount = 0, uint8_t totalFound = 0);
36|    static void resetBLESniffState();  // Reset first-target sniff flag on mode start
37|    
38|    // Get current mood phrase
39|    static const char* getCurrentPhrase();
40|    static int getCurrentHappiness();
41|    static int getEffectiveHappiness();  // Happiness with momentum applied
42|    static int getLastEffectiveHappiness();  // Cached effective happiness (no decay)
43|    static uint32_t getLastActivityTime();  // For buff/debuff idle detection
44|    static void adjustHappiness(int delta);  // Direct happiness adjustment
45|    
46|    // Dialogue lock - prevents automatic phrase selection during BLE sync dialogue
47|    static void setDialogueLock(bool locked);
48|    static bool isDialogueLocked();
49|    
50|    // Phase 6: Public for phrase chaining helper functions
51|    static char currentPhrase[40];
52|    static uint32_t lastPhraseChange;
53|    static char phraseQueue[4][40];  // Expanded for 5-line riddles
54|    static uint8_t phraseQueueCount;
55|    static uint32_t lastQueuePop;
56|    
57|private:
58|    static int happiness;  // -100 to 100 (base level)
59|    static uint32_t phraseInterval;
60|    static uint32_t lastActivityTime;
61|
62|    // Mood momentum system - recent boosts decay over time
63|    static int momentumBoost;           // Current boost amount (decays)
64|    static uint32_t lastBoostTime;      // When boost was applied
65|    static const uint32_t MOMENTUM_DECAY_MS = 30000;  // 30s full decay
66|
67|    static void selectPhrase();
68|    static void updateAvatarState();
69|    static void applyMomentumBoost(int amount);
70|    static void decayMomentum();
71|
72|    // === Situational Awareness State ===
73|    static void updateSituationalAwareness(uint32_t now);
74|    static bool pickTimePhraseIfDue(uint32_t now);
75|    static bool pickHeapPhraseIfDue(uint32_t now);
76|    static bool pickDensityPhraseIfDue(uint32_t now);
77|    static bool pickChallengePhraseIfDue(uint32_t now);
78|    static bool pickGPSPhraseIfDue(uint32_t now);
79|    static bool pickFatiguePhraseIfDue(uint32_t now);
80|    static bool pickEncryptionPhraseIfDue(uint32_t now);
81|    static bool pickBuffPhraseIfDue(uint32_t now);
82|    static bool pickChargingPhraseIfDue(uint32_t now);
83|    static bool pickWeatherPhraseIfDue(uint32_t now);
84|};
85|