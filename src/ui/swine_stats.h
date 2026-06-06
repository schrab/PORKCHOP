1|// SWINE STATS - Lifetime statistics and active buff/debuff overlay
2|#pragma once
3|
4|5|
6|// Buff/Debuff flags (can have multiple active)
7|enum class PorkBuff : uint8_t {
8|    NONE = 0,
9|    // Buffs (positive effects)
10|    // vNext Neon Operator buffs (see swine_stats.cpp for exact effects)
11|    R4G3           = (1 << 0),  // NE0N H1GH: -18% Street Sweep; faster mood decay
12|    SNOUT_SHARP    = (1 << 1),  // SNOUT$HARP: +18% global XP gain
13|    H0TSTR3AK      = (1 << 2),  // H0TSTR3AK: +6% capture XP when on a streak
14|    C4FF31N4T3D    = (1 << 3),  // C0LD F0CU5: +10% Glass Stare, +5% Street Sweep
15|    CL34R_SKY      = (1 << 4),  // CL34R$KY: +5% Signal Drip in clear weather
16|};
17|
18|enum class PorkDebuff : uint8_t {
19|    NONE = 0,
20|    // Debuffs (negative effects)
21|    // vNext Neon Operator debuffs
22|    SLOP_SLUG      = (1 << 0),  // SLOP$LUG: +12% Street Sweep (slower scans) when very unhappy
23|    F0GSNOUT       = (1 << 1),  // F0GSNOUT: -10% XP gain when a bit unhappy
24|    TR0UGHDR41N    = (1 << 2),  // TR0UGHDR41N: +1ms jitter after inactivity
25|    HAM_STR1NG     = (1 << 3),  // HAM$TR1NG: +35% Street Sweep when extremely unhappy
26|    TH0ND3R_SLAB   = (1 << 4),  // TH0ND3R$LAB: +8% Street Sweep during storms
27|};
28|
29|// Class buff flags (permanent, cumulative based on level)
30|enum class ClassBuff : uint16_t {
31|    NONE         = 0,
32|    P4CK3T_NOSE  = (1 << 0),  // A1R R34D3R (SN1FF3R L6+): -8% Street Sweep
33|    H4RD_SNOUT   = (1 << 1),  // T4RG3T F0CU5 (PWNER L11+): +0.6s Glass Stare
34|    R04D_H0G     = (1 << 2),  // R04M CR3D (R00T L16+): +12% distance XP
35|    SH4RP_TUSKS  = (1 << 3),  // GL4SS ST4R3+ (R0GU3 L21+): +0.8s Glass Stare
36|    CR4CK_NOSE   = (1 << 4),  // L00T M3M0RY (EXPL01T L26+): +10% capture XP
37|    IR0N_TUSKS   = (1 << 5),  // CL0CK NERV3S (WARL0RD L31+): -10% jitter (Clock Nerves)
38|    OMNI_P0RK    = (1 << 6),  // 0MN1P0RK (L3G3ND L36+): +4% to all modifiers
39|    K3RN3L_H0G   = (1 << 7),  // PR0T0C0L 5EER (L41+): +6% cap/dist XP
40|    B4C0NM4NC3R  = (1 << 8)   // B4C0N 0V3RDR1V3 (L46+): +8% cap/dist XP
41|};
42|
43|// Active buff/debuff state
44|struct BuffState {
45|    uint8_t buffs;    // PorkBuff flags
46|    uint8_t debuffs;  // PorkDebuff flags
47|    
48|    bool hasBuff(PorkBuff b) const { return buffs & (uint8_t)b; }
49|    bool hasDebuff(PorkDebuff d) const { return debuffs & (uint8_t)d; }
50|};
51|
52|// Tab selection for SWINE STATS
53|enum class StatsTab : uint8_t {
54|    STATS = 0,
55|    BOOSTS = 1,
56|    WIGLE = 2
57|};
58|
59|class SwineStats {
60|public:
61|    static void init();
62|    static void show();
63|    static void hide();
64|    static void update();
65|    static void draw(M5Canvas& canvas);
66|    static bool isActive() { return active; }
67|    
68|    // Buff/debuff calculation (called by modes)
69|    static BuffState calculateBuffs();
70|    static uint16_t calculateClassBuffs();  // Returns ClassBuff flags
71|    
72|    // Buff effect getters for game mechanics
73|    static uint8_t getDeauthBurstCount();     // Base 5, modified by buffs
74|    static uint8_t getDeauthJitterMax();      // Base 5ms, modified by debuffs
75|    static uint16_t getChannelHopInterval();  // Base from config, modified
76|    static float getXPMultiplier();           // 1.0 base, modified
77|    static uint32_t getLockTime();            // Base 4000ms (configurable), modified by class
78|    static float getDistanceXPMultiplier();   // 1.0 base, modified by class
79|    static float getCaptureXPMultiplier();    // 1.0 base, modified by class
80|    
81|    // Class buff helpers
82|    static bool hasClassBuff(ClassBuff cb);
83|    static const char* getClassBuffName(ClassBuff cb);
84|    static const char* getClassBuffDesc(ClassBuff cb);
85|    
86|    // Buff/debuff name getters for display
87|    static const char* getBuffName(PorkBuff b);
88|    static const char* getDebuffName(PorkDebuff d);
89|    static const char* getBuffDesc(PorkBuff b);
90|    static const char* getDebuffDesc(PorkDebuff d);
91|
92|private:
93|    static bool active;
94|    static bool keyWasPressed;
95|    static BuffState currentBuffs;
96|    static uint16_t currentClassBuffs;
97|    static uint32_t lastBuffUpdate;
98|    static StatsTab currentTab;
99|    
100|    static void handleInput();
101|    static void drawStatsTab(M5Canvas& canvas);
102|    static void drawBuffsTab(M5Canvas& canvas);
103|    static void drawTabBar(M5Canvas& canvas);
104|    static void drawStats(M5Canvas& canvas);  // Stat grid helper
105|
106|    // Draw the WiGLE statistics tab. This tab displays the user's rank
107|    // and total observations as read from the WiGLE stats cache. See
108|    // WiGLE::getUserStats() for details.
109|    static void drawWigleTab(M5Canvas& canvas);
110|};