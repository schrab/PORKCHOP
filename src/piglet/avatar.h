1|// Piglet ASCII avatar
2|#pragma once
3|
4|5|
6|enum class AvatarState {
7|    NEUTRAL,
8|    HAPPY,
9|    EXCITED,
10|    HUNTING,
11|    SLEEPY,
12|    SAD,
13|    ANGRY
14|};
15|
16|class Avatar {
17|public:
18|    static void init();
19|    static void draw(M5Canvas& canvas);
20|    static void setState(AvatarState state);
21|    static AvatarState getState() { return currentState; }
22|    static bool isFacingRight();  // Get current facing direction
23|    static bool isOnRightSide();  // Get screen position (for bubble placement)
24|    static bool isTransitioning();  // True during walk transition (hide bubble)
25|    static int getCurrentX();  // Get current animated X position
26|    
27|    // Phase 8: Intensity-based animation modifiers
28|    static void setMoodIntensity(int intensity);  // -100 to 100, affects blink/flip rates
29|    
30|    static void blink();
31|    static void sniff();  // Trigger nose sniff animation (600ms animated cycle)
32|    static void wiggleEars();
33|    static void cuteJump();  // Trigger cute celebratory jump (higher than walk bounce)
34|    
35|    // Direction control
36|    static void setFacingLeft();
37|    static void setFacingRight();
38|    
39|    // Attack shake (visual feedback for captures)
40|    static void setAttackShake(bool active, bool strong);
41|    
42|    // Thunder flash (invert colors for weather effect)
43|    static void setThunderFlash(bool active);
44|    static bool isThunderFlashing();
45|
46|    // Night sky star system (RTC-based)
47|    static bool isNightTime();           // check rtc for night hours, 20:00-06:00
48|    static bool areStarsActive();        // stars currently visible?
49|    
50|    // Walk wind-up animation (smooth slide for coast-back)
51|    static void startWindupSlide(int targetX, bool faceRight = false);
52|    
53|    // Grass animation control (direction: true=right, false=left)
54|    static void setGrassMoving(bool moving, bool directionRight = true);
55|    static bool isGrassMoving() { return grassMoving; }
56|    static bool isGrassDirectionRight() { return grassDirection; }
57|    static uint16_t getGrassSpeed() { return grassSpeed; }
58|    static void setGrassSpeed(uint16_t ms);  // Speed in ms per shift (lower = faster)
59|    static void setGrassPattern(const char* pattern);  // Custom pattern (max 26 chars)
60|    static void resetGrassPattern();  // Reset to random binary pattern
61|    
62|private:
63|    // Star system state
64|    struct Star {
65|        int16_t x;              // screen x range 0-239
66|        int16_t y;              // screen y range 20-100
67|        uint8_t size;           // 1-2 px radius
68|        uint8_t brightness;     // 0-255, 0 means hidden
69|        bool isBlinking;        // twinkle behavior
70|        uint32_t fadeInStart;   // when this star started appearing
71|    };
72|    static Star stars[15];
73|    static uint8_t starCount;
74|    static constexpr uint8_t MAX_STARS = 15;
75|    static uint32_t lastStarSpawn;
76|    static uint32_t nextSpawnDelay;
77|    static bool starsActive;
78|    static uint32_t lastNightCheck;
79|    static bool cachedNightMode;
80|
81|    static void initStarPositions();
82|    static void updateStars();
83|    static void drawStars(M5Canvas& canvas);
84|    static void fillPigBoundingBox(M5Canvas& canvas);
85|    static AvatarState currentState;
86|    static bool isBlinking;
87|    static bool isSniffing;
88|    static bool earsUp;
89|    static uint32_t lastBlinkTime;
90|    static uint32_t blinkInterval;
91|    static int moodIntensity;  // Phase 8: -100 to 100
92|    
93|    // Cute jump animation state
94|    static bool jumpActive;
95|    static uint32_t jumpStartTime;
96|    static constexpr uint16_t JUMP_DURATION_MS = 400;  // Total jump time (up + down)
97|    static constexpr int JUMP_HEIGHT = 8;  // Pixels to jump up
98|    
99|    // Walk transition animation
100|    static bool transitioning;
101|    static uint32_t transitionStartTime;
102|    static int transitionFromX;
103|    static int transitionToX;
104|    static bool transitionToFacingRight;
105|    static int currentX;  // Animated X position
106|    static constexpr uint16_t TRANSITION_DURATION_MS = 400;  // Walk across time
107|    
108|    // Grass animation state
109|    static bool grassMoving;
110|    static bool grassDirection;  // true = grass scrolls right, false = scrolls left
111|    static bool pendingGrassStart;  // Wait for transition before starting grass
112|    static bool onRightSide;  // Track which side of screen pig is on
113|    static uint32_t lastGrassUpdate;
114|    static uint16_t grassSpeed;  // ms per shift
115|    static char grassPattern[32];  // Wider for full screen coverage
116|    
117|    static void drawFrame(M5Canvas& canvas, const char** frame, uint8_t lines, bool blink = false, bool faceRight = true, bool sniff = false);
118|    static void drawGrass(M5Canvas& canvas);
119|    static void updateGrass();
120|};
121|