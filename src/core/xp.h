1|// Porkchop RPG XP and Leveling System
2|#pragma once
3|
4|5|#include <Preferences.h>
6|
7|// Class tiers (every 5 levels)
8|enum class PorkClass : uint8_t {
9|    SH0AT    = 0,   // L1–5  : fresh firmware, newbie
10|    SN1FF3R  = 1,   // L6–10 : packet sniffer
11|    PWNER    = 2,   // L11–15: PR0B3R archetype (probe/scout)
12|    R00T     = 3,   // L16–20: PWN3R archetype (first real exploits)
13|    R0GU3    = 4,   // L21–25: H4ND5H4K3R archetype (handshake hunter)
14|    EXPL01T  = 5,   // L26–30: M1TM B0AR archetype (man‑in‑the‑middle)
15|    WARL0RD  = 6,   // L31–35: R00T BR1STL3 archetype (root‑level bristles)
16|    L3G3ND   = 7,   // L36–40: PMF W4RD3N archetype (PMF savvy)
17|    K3RN3L_H0G = 8, // L41–45: MLO L3G3ND archetype (multi‑link legend)
18|    B4C0NM4NC3R = 9 // L46–50: endgame myth (B4C0NM4NC3R)
19|};
20|
21|// Title overrides - special playstyle-based titles
22|enum class TitleOverride : uint8_t {
23|    NONE          = 0,   // Use standard level-based rank
24|    SH4D0W_H4M    = 1,   // Unlocked by ACH_SHADOW_BROKER (500 passive nets)
25|    P4C1F1ST_P0RK = 2,   // Unlocked by ACH_WITNESS_PROTECT (25 bros)
26|    Z3N_M4ST3R    = 3    // Unlocked by ACH_ZEN_MASTER
27|};
28|
29|// XP event types for tracking
30|enum class XPEvent : uint8_t {
31|    NETWORK_FOUND,          // +1 XP
32|    NETWORK_HIDDEN,         // +3 XP
33|    NETWORK_WPA3,           // +10 XP
34|    NETWORK_OPEN,           // +3 XP
35|    NETWORK_WEP,            // +5 XP (rare find!)
36|    HANDSHAKE_CAPTURED,     // +50 XP
37|    PMKID_CAPTURED,         // +75 XP
38|    DEAUTH_SENT,            // +1 XP  (vNext: reward restraint)
39|    DEAUTH_SUCCESS,         // +15 XP
40|    WARHOG_LOGGED,          // +1 XP  (vNext: passive drive nerf)
41|    DISTANCE_KM,            // +30 XP (vNext: buff physical effort)
42|    BLE_BURST,              // +1 XP  (vNext: nerfed spam)
43|    BLE_APPLE,              // +2 XP  (vNext: nerfed spam)
44|    BLE_ANDROID,            // +1 XP  (vNext: nerfed spam)
45|    BLE_SAMSUNG,            // +1 XP  (vNext: nerfed spam)
46|    BLE_WINDOWS,            // +1 XP  (vNext: nerfed spam)
47|    GPS_LOCK,               // +5 XP
48|    ML_ROGUE_DETECTED,      // +25 XP
49|    SESSION_30MIN,          // +10 XP
50|    SESSION_60MIN,          // +25 XP
51|    SESSION_120MIN,         // +50 XP
52|    LOW_BATTERY_CAPTURE,    // +20 XP bonus
53|    // DO NO HAM / BOAR BROS events (v0.1.4+)
54|    DNH_NETWORK_PASSIVE,    // +2 XP - network found in passive mode
55|    DNH_PMKID_GHOST,        // +150 XP (vNext: very rare passive!)
56|    BOAR_BRO_ADDED,         // +5 XP - added network to BOAR BROS
57|    BOAR_BRO_MERCY,         // +15 XP - excluded mid-attack target
58|    SMOKED_BACON            // +15 XP - rare upload bonus
59|};
60|
61|// Achievement bitflags (uint64_t for 60 achievements)
62|enum PorkAchievement : uint64_t {
63|    ACH_NONE            = 0,
64|    // Original 17 achievements (bits 0-16)
65|    ACH_FIRST_BLOOD     = 1ULL << 0,   // First handshake
66|    ACH_CENTURION       = 1ULL << 1,   // 100 networks in one session
67|    ACH_MARATHON_PIG    = 1ULL << 2,   // 10km walked in session
68|    ACH_NIGHT_OWL       = 1ULL << 3,   // Session after midnight
69|    ACH_GHOST_HUNTER    = 1ULL << 4,   // 10 hidden networks
70|    ACH_APPLE_FARMER    = 1ULL << 5,   // 100 Apple BLE hits
71|    ACH_WARDRIVER       = 1ULL << 6,   // 1000 lifetime networks
72|    ACH_DEAUTH_KING     = 1ULL << 7,   // 100 successful deauths
73|    ACH_PMKID_HUNTER    = 1ULL << 8,   // Capture PMKID
74|    ACH_WPA3_SPOTTER    = 1ULL << 9,   // Find WPA3 network
75|    ACH_GPS_MASTER      = 1ULL << 10,  // 100 GPS-tagged networks
76|    ACH_TOUCH_GRASS     = 1ULL << 11,  // 50km total walked
77|    ACH_SILICON_PSYCHO  = 1ULL << 12,  // 5000 lifetime networks
78|    ACH_CLUTCH_CAPTURE  = 1ULL << 13,  // Handshake at <10% battery
79|    ACH_SPEED_RUN       = 1ULL << 14,  // 50 networks in 10 minutes
80|    ACH_CHAOS_AGENT     = 1ULL << 15,  // 1000 BLE packets sent
81|    ACH_NIETZSWINE      = 1ULL << 16,  // Stare at spectrum for 15 minutes
82|    
83|    // New achievements (bits 17-46)
84|    // Network milestones
85|    ACH_TEN_THOUSAND    = 1ULL << 17,  // 10,000 networks lifetime
86|    ACH_NEWB_SNIFFER    = 1ULL << 18,  // First 10 networks
87|    ACH_FIVE_HUNDRED    = 1ULL << 19,  // 500 networks in session
88|    ACH_OPEN_SEASON     = 1ULL << 20,  // 50 open networks
89|    ACH_WEP_LOLZER      = 1ULL << 21,  // Find a WEP network
90|    
91|    // Handshake/PMKID milestones
92|    ACH_HANDSHAKE_HAM   = 1ULL << 22,  // 10 handshakes lifetime
93|    ACH_FIFTY_SHAKES    = 1ULL << 23,  // 50 handshakes lifetime
94|    ACH_PMKID_FIEND     = 1ULL << 24,  // 10 PMKIDs captured
95|    ACH_TRIPLE_THREAT   = 1ULL << 25,  // 3 handshakes in session
96|    ACH_HOT_STREAK      = 1ULL << 26,  // 5 handshakes in session
97|    
98|    // Deauth milestones
99|    ACH_FIRST_DEAUTH    = 1ULL << 27,  // First successful deauth
100|    ACH_DEAUTH_THOUSAND = 1ULL << 28,  // 1000 successful deauths
101|    ACH_RAMPAGE         = 1ULL << 29,  // 10 deauths in session
102|    
103|    // Distance/WARHOG milestones
104|    ACH_HALF_MARATHON   = 1ULL << 30,  // 21km in session
105|    ACH_HUNDRED_KM      = 1ULL << 31,  // 100km lifetime
106|    ACH_GPS_ADDICT      = 1ULL << 32,  // 500 GPS-tagged networks
107|    ACH_ULTRAMARATHON   = 1ULL << 33,  // 42.195km in session (actual marathon)
108|    
109|    // BLE/PIGGYBLUES milestones
110|    ACH_PARANOID_ANDROID = 1ULL << 34, // 100 Android FastPair spam
111|    ACH_SAMSUNG_SPRAY   = 1ULL << 35,  // 100 Samsung spam
112|    ACH_WINDOWS_PANIC   = 1ULL << 36,  // 100 Windows SwiftPair spam
113|    ACH_BLE_BOMBER      = 1ULL << 37,  // 5000 BLE packets
114|    ACH_OINKAGEDDON     = 1ULL << 38,  // 10000 BLE packets
115|    
116|    // Time/session milestones
117|    ACH_SESSION_VET     = 1ULL << 39,  // 100 sessions
118|    ACH_FOUR_HOUR_GRIND = 1ULL << 40,  // 4 hour session
119|    ACH_EARLY_BIRD      = 1ULL << 41,  // Active 5-7am
120|    ACH_WEEKEND_WARRIOR = 1ULL << 42,  // Session on weekend
121|    
122|    // Special/rare
123|    ACH_ROGUE_SPOTTER   = 1ULL << 43,  // ML detects rogue AP
124|    ACH_HIDDEN_MASTER   = 1ULL << 44,  // 50 hidden networks
125|    ACH_WPA3_HUNTER     = 1ULL << 45,  // 25 WPA3 networks
126|    ACH_MAX_LEVEL       = 1ULL << 46,  // Reach level 50
127|    ACH_ABOUT_JUNKIE    = 1ULL << 47,  // Press Enter 5x in About screen
128|    
129|    // DO NO HAM achievements (bits 48-52) - pacifist/stealth playstyle
130|    ACH_GOING_DARK      = 1ULL << 48,  // 5 minutes in passive mode this session
131|    ACH_GHOST_PROTOCOL  = 1ULL << 49,  // 30 min passive + 50 networks in session
132|    ACH_SHADOW_BROKER   = 1ULL << 50,  // 500 passive networks lifetime (unlocks SH4D0W_H4M)
133|    ACH_SILENT_ASSASSIN = 1ULL << 51,  // First PMKID captured in passive mode
134|    ACH_ZEN_MASTER      = 1ULL << 52,  // 5 passive PMKIDs (unlocks Z3N_M4ST3R title)
135|    
136|    // BOAR BROS achievements (bits 53-57) - network protection playstyle
137|    ACH_FIRST_BRO       = 1ULL << 53,  // First network added to BOAR BROS
138|    ACH_FIVE_FAMILIES   = 1ULL << 54,  // 5 bros added lifetime
139|    ACH_MERCY_MODE      = 1ULL << 55,  // First mid-attack exclusion
140|    ACH_WITNESS_PROTECT = 1ULL << 56,  // 25 bros added lifetime (unlocks P4C1F1ST_P0RK)
141|    ACH_FULL_ROSTER     = 1ULL << 57,  // Currently have 50 bros (max limit)
142|    
143|    // Lore achievement (bit 58) - v0.1.8
144|    ACH_PROPHECY_WITNESS = 1ULL << 58,  // Witnessed the riddle prophecy
145|    
146|    // Combined DO NO HAM + BOAR BROS achievements (bit 59)
147|    ACH_PACIFIST_RUN    = 1ULL << 59,  // 50+ networks discovered, all added to bros
148|    
149|    // CLIENT MONITOR achievements (bits 60-62) - v0.1.6 hunting features
150|    ACH_QUICK_DRAW      = 1ULL << 60,  // Deauth 5 clients in under 30 seconds
151|    ACH_DEAD_EYE        = 1ULL << 61,  // Deauth within 2 seconds of entering monitor
152|    ACH_HIGH_NOON       = 1ULL << 62,  // Deauth during 12:00 hour (noon)
153|    
154|    // Ultimate achievement (bit 63) - v0.1.8
155|    ACH_FULL_CLEAR      = 1ULL << 63,  // All other achievements unlocked (TH3_C0MPL3T10N1ST)
156|};
157|
158|// Persistent XP data structure (stored in NVS)
159|struct PorkXPData {
160|    uint32_t totalXP;           // Lifetime XP
161|    uint64_t achievements;      // Achievement bitfield (expanded for 60 achievements)
162|    uint32_t lifetimeNetworks;  // Counter
163|    uint32_t lifetimeHS;        // Counter
164|    uint32_t lifetimePMKID;     // PMKID counter
165|    uint32_t lifetimeDeauths;   // Counter
166|    uint32_t lifetimeDistance;  // Meters
167|    uint32_t lifetimeBLE;       // BLE packets
168|    uint32_t hiddenNetworks;    // Hidden network count
169|    uint32_t wpa3Networks;      // WPA3 network count
170|    uint32_t gpsNetworks;       // GPS-tagged networks
171|    uint32_t openNetworks;      // Open network count (new)
172|    uint32_t androidBLE;        // Android FastPair count (new)
173|    uint32_t samsungBLE;        // Samsung BLE count (new)
174|    uint32_t windowsBLE;        // Windows SwiftPair count (new)
175|    uint32_t rouletteWins;      // PiggyBlues no-reboot roulette wins
176|    uint16_t sessions;          // Session count
177|    uint8_t  cachedLevel;       // Cached level for quick access
178|    bool     wepFound;          // WEP network ever found (new)
179|    // DO NO HAM / BOAR BROS persistent counters (v0.1.4+)
180|    uint32_t passiveNetworks;   // Networks found in DNH mode
181|    uint32_t passivePMKIDs;     // PMKIDs captured in DNH mode
182|    uint32_t passiveTimeS;      // Seconds in pure passive mode (no deauth ever)
183|    uint32_t boarBrosAdded;     // Total networks added to BOAR BROS
184|    uint32_t mercyCount;        // Mid-attack exclusions (mercy kills)
185|    TitleOverride titleOverride; // Player-selected title override
186|    uint32_t unlockables;       // Unlockables bitfield (v0.1.8) - secret challenges
187|};
188|
189|// Session-only stats (not persisted)
190|struct SessionStats {
191|    uint32_t xp;
192|    uint32_t networks;
193|    uint32_t handshakes;
194|    uint32_t deauths;
195|    uint32_t distanceM;
196|    uint32_t blePackets;
197|    uint32_t startTime;
198|    uint32_t firstNetworkTime;  // Time first network was found (for speed run)
199|    bool gpsLockAwarded;
200|    bool session30Awarded;
201|    bool session60Awarded;
202|    bool session120Awarded;
203|    bool nightOwlAwarded;       // Hunt after midnight
204|    bool session240Awarded;     // 4 hour session (new)
205|    bool earlyBirdAwarded;      // 5-7am session (new)
206|    bool weekendWarriorAwarded; // Weekend session (new)
207|    bool rogueSpotterAwarded;   // ML rogue detected (new)
208|    // DO NO HAM / BOAR BROS session counters (v0.1.4+)
209|    uint32_t passiveNetworks;   // Networks in DNH mode this session
210|    uint32_t passivePMKIDs;     // PMKIDs in DNH mode this session
211|    uint32_t passiveTimeStart;  // millis() when DNH enabled (0 = not in DNH)
212|    uint32_t boarBrosThisSession; // Bros added this session (for PACIFIST_RUN)
213|    uint32_t mercyCount;        // Mid-attack exclusions this session
214|    bool everDeauthed;          // Has player ever sent deauth? (for Silent Witness)
215|};
216|
217|class XP {
218|public:
219|    static void init();
220|    static void save();
221|    static void processPendingSave();  // Process deferred saves (call from safe context)
222|    static void processAchievementQueue();  // Process one queued achievement celebration
223|    
224|    // XP operations
225|    static void addXP(XPEvent event);
226|    static void addXP(uint16_t amount);  // Direct XP add (can trigger JACKPOT)
227|    static void addXPSilent(uint16_t amount);  // Silent XP add (no JACKPOT, no toast)
228|    static void addRouletteWin();  // PiggyBlues no-reboot roulette counter
229|    
230|    // Level info
231|    static uint8_t getLevel();
232|    static uint32_t getTotalXP();
233|    static uint32_t getXPForLevel(uint8_t level);
234|    static uint32_t getXPToNextLevel();
235|    static uint8_t getProgress();  // 0-100%
236|    static const char* getTitle();
237|    static const char* getTitleForLevel(uint8_t level);
238|    
239|    // Title override system (v0.1.4+)
240|    static const char* getDisplayTitle();  // Returns override title if set, else level title
241|    static TitleOverride getTitleOverride();
242|    static void setTitleOverride(TitleOverride override);
243|    static const char* getTitleOverrideName(TitleOverride override);
244|    static bool canUseTitleOverride(TitleOverride override);  // Check if player has unlocked it
245|    static TitleOverride getNextAvailableOverride();  // Cycle through unlocked overrides
246|    
247|    // Class info
248|    static PorkClass getClass();
249|    static PorkClass getClassForLevel(uint8_t level);
250|    static const char* getClassName();
251|    static const char* getClassNameFor(PorkClass cls);
252|    static uint8_t getClassIndex();  // 0-9
253|    
254|    // Achievements
255|    static void unlockAchievement(PorkAchievement ach);
256|    static bool hasAchievement(PorkAchievement ach);
257|    static uint64_t getAchievements();
258|    static uint8_t getUnlockedCount();  // Count of unlocked achievements
259|    static uint8_t getAchievementCount();  // Total achievement count
260|    static const char* getAchievementName(PorkAchievement ach);
261|    
262|    // Unlockables (v0.1.8) - secret challenges
263|    static void setUnlockable(uint8_t bitIndex);
264|    static bool hasUnlockable(uint8_t bitIndex);
265|    static uint32_t getUnlockables();
266|    
267|    // Stats access
268|    static const PorkXPData& getData();
269|    static const SessionStats& getSession();
270|    
271|    // Session management
272|    static void startSession();
273|    static void endSession();
274|    static void updateSessionTime();  // Check time-based bonuses
275|    
276|    // Distance tracking (call from WARHOG)
277|    static void addDistance(uint32_t meters);
278|    
279|    // Draw XP bar on canvas
280|    static void drawBar(DisplayCanvas& canvas);
281|    
282|    // XP notification for top bar (Option B: flash on gain)
283|    static bool shouldShowXPNotification();  // True if within 5 sec of last XP gain
284|    static void drawTopBarXP(DisplayCanvas& topBar);  // Draw inverted XP info on top bar
285|    static uint16_t getLastXPGainAmount();
286|    
287|    // Level up callback (set by display to show popup)
288|    static void setLevelUpCallback(void (*callback)(uint8_t oldLevel, uint8_t newLevel));
289|
290|private:
291|    static PorkXPData data;
292|    static SessionStats session;
293|    static Preferences prefs;
294|    static bool initialized;
295|    static void (*levelUpCallback)(uint8_t, uint8_t);
296|    
297|    static void load();
298|    static void checkAchievements();
299|    static uint8_t calculateLevel(uint32_t xp);
300|    
301|    // SD backup - immortal pig survives M5Burner
302|    static bool backupToSD();
303|    static bool restoreFromSD();
304|};
305|