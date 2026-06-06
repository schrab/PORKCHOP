1|// Bounty Status Menu - View bounties to send to kid (Sirloin)
2|// Porkchop sends wardriven networks TO Sirloin for hunting
3|// Refactored to match captures_menu/boar_bros_menu patterns
4|
5|#include "bounty_status_menu.h"
6|#include "../hal/hal_input.h"
#include "../hal/hal_display.h"
7|#include "display.h"
8|#include "../modes/pigsync_client.h"
9|#include "../modes/warhog.h"
10|
11|// Static member initialization
12|uint16_t BountyStatusMenu::selectedIndex = 0;
13|uint16_t BountyStatusMenu::scrollOffset = 0;
14|bool BountyStatusMenu::active = false;
15|bool BountyStatusMenu::keyWasPressed = false;
16|
17|// Cached bounty list (refreshed each draw cycle to avoid 5x redundant calls)
18|static std::vector<uint64_t> cachedBounties;
19|static const uint32_t BOUNTY_CACHE_REFRESH_MS = 1000;
20|static uint32_t lastCacheRefreshMs = 0;
21|static bool cacheDirty = true;
22|
23|static void refreshBountyCache(bool force) {
24|    uint32_t now = millis();
25|    if (!force && !cacheDirty && (now - lastCacheRefreshMs) < BOUNTY_CACHE_REFRESH_MS) {
26|        return;
27|    }
28|    cachedBounties = WarhogMode::getUnclaimedBSSIDs();
29|    lastCacheRefreshMs = now;
30|    cacheDirty = false;
31|}
32|
33|// Format uint64_t BSSID to string
34|static void formatBSSID(uint64_t bssid, char* out, size_t len) {
35|    if (!out || len == 0) return;
36|    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
37|             (uint8_t)(bssid >> 40),
38|             (uint8_t)(bssid >> 32),
39|             (uint8_t)(bssid >> 24),
40|             (uint8_t)(bssid >> 16),
41|             (uint8_t)(bssid >> 8),
42|             (uint8_t)(bssid));
43|}
44|
45|void BountyStatusMenu::init() {
46|    selectedIndex = 0;
47|    scrollOffset = 0;
48|}
49|
50|void BountyStatusMenu::show() {
51|    active = true;
52|    selectedIndex = 0;
53|    scrollOffset = 0;
54|    keyWasPressed = true;  // Ignore the Enter that opened us
55|    cacheDirty = true;
56|    lastCacheRefreshMs = 0;
57|    refreshBountyCache(true);
58|}
59|
60|void BountyStatusMenu::hide() {
61|    active = false;
62|    cachedBounties.clear();
63|    cachedBounties.shrink_to_fit();  // FIX: Return capacity to heap, avoid fragmentation
64|    cacheDirty = true;
65|    lastCacheRefreshMs = 0;
66|}
67|
68|void BountyStatusMenu::getSelectedInfo(char* out, size_t len) {
69|    if (!out || len == 0) return;
70|    refreshBountyCache(false);
71|    size_t readyCount = cachedBounties.size();
72|    
73|    // Get sync stats for bottom bar
74|    uint16_t totalSynced = PigSyncMode::getTotalSynced();
75|    uint8_t claimedCount = PigSyncMode::getLastBountyMatches();
76|    
77|    // Format: RDY:XX SYNC:XX CLMD:XX
78|    snprintf(out, len, "RDY:%u SYNC:%u CLMD:%u",
79|             (unsigned)readyCount, (unsigned)totalSynced, (unsigned)claimedCount);
80|}
81|
82|void BountyStatusMenu::update() {
83|    if (!active) return;
84|    handleInput();
85|}
86|
87|void BountyStatusMenu::handleInput() {
88|    bool anyPressed = hal_input_anyHeld();
89|    
90|    if (!anyPressed) {
91|        keyWasPressed = false;
92|        return;
93|    }
94|    
95|    if (keyWasPressed) return;
96|    keyWasPressed = true;
97|    
98|    // Use cached bounties (refreshed by draw() each frame)
99|    size_t count = cachedBounties.size();
100|    
101|    // Navigation (; = up, . = down)
102|    if (hal_input_wasPressed(KEY_UP)) {
103|        if (selectedIndex > 0) {
104|            selectedIndex--;
105|            if (selectedIndex < scrollOffset) {
106|                scrollOffset = selectedIndex;
107|            }
108|        }
109|    }
110|    
111|    if (hal_input_wasPressed(KEY_DOWN)) {
112|        if (count > 0 && selectedIndex < count - 1) {
113|            selectedIndex++;
114|            if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
115|                scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
116|            }
117|        }
118|    }
119|    
120|    // Backspace - go back
121|    if (hal_input_wasPressed(KEY_BACKSPACE)) {
122|        hide();
123|    }
124|}
125|
126|void BountyStatusMenu::draw(M5Canvas& canvas) {
127|    canvas.fillSprite(COLOR_BG);
128|    canvas.setTextColor(COLOR_FG);
129|    canvas.setTextSize(1);
130|    
131|    // Refresh cache on a timer to avoid per-frame allocations
132|    refreshBountyCache(false);
133|    
134|    if (cachedBounties.empty()) {
135|        drawEmpty(canvas);
136|    } else {
137|        drawList(canvas);
138|    }
139|}
140|
141|void BountyStatusMenu::drawList(M5Canvas& canvas) {
142|    // Uses cached bounties from draw()
143|    size_t count = cachedBounties.size();
144|    
145|    if (count == 0) return;
146|    
147|    // Bounds check (handle case where list shrank since last frame)
148|    if (selectedIndex >= count) {
149|        selectedIndex = (uint16_t)(count - 1);
150|    }
151|    if (scrollOffset > selectedIndex) {
152|        scrollOffset = selectedIndex;
153|    }
154|    
155|    // Start at top of canvas (no header)
156|    int y = 2;
157|    
158|    // Loop with uint16_t to support >255 items
159|    uint16_t endIdx = (uint16_t)min((size_t)(scrollOffset + VISIBLE_ITEMS), count);
160|    for (uint16_t i = scrollOffset; i < endIdx; i++) {
161|        uint64_t bssid = cachedBounties[i];
162|        char bssidStr[20];
163|        formatBSSID(bssid, bssidStr, sizeof(bssidStr));
164|        
165|        // Highlight selected (standard porkchop pattern - inverted colors)
166|        if (i == selectedIndex) {
167|            canvas.fillRect(0, y - 1, canvas.width(), LINE_H, COLOR_FG);
168|            canvas.setTextColor(COLOR_BG);
169|        } else {
170|            canvas.setTextColor(COLOR_FG);
171|        }
172|        
173|        // BSSID (clean, left-aligned)
174|        canvas.setCursor(COL_LEFT, y);
175|        canvas.print(bssidStr);
176|        
177|        y += LINE_H;
178|    }
179|    
180|    // Scroll indicators (standard pattern)
181|    canvas.setTextColor(COLOR_FG);
182|    if (scrollOffset > 0) {
183|        canvas.setCursor(canvas.width() - 10, 2);
184|        canvas.print("^");
185|    }
186|    if (scrollOffset + VISIBLE_ITEMS < count) {
187|        canvas.setCursor(canvas.width() - 10, 2 + (VISIBLE_ITEMS - 1) * LINE_H);
188|        canvas.print("v");
189|    }
190|}
191|
192|void BountyStatusMenu::drawEmpty(M5Canvas& canvas) {
193|    // Toast-style empty state (centered rounded box, inverted colors)
194|    const int boxW = 180;
195|    const int boxH = 50;
196|    const int boxX = (canvas.width() - boxW) / 2;
197|    const int boxY = (canvas.height() - boxH) / 2 - 5;
198|    
199|    // Black border then pink fill (inverted colors)
200|    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
201|    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
202|    
203|    // Black text on pink background
204|    canvas.setTextColor(COLOR_BG, COLOR_FG);
205|    canvas.setTextDatum(MC_DATUM);
206|    
207|    canvas.drawString("N0 B0UNT13S Y3T!", canvas.width() / 2, boxY + 15);
208|    canvas.drawString("RUN W4RH0G [W] T0 HUNT", canvas.width() / 2, boxY + 35);
209|    
210|    // Reset text state
211|    canvas.setTextDatum(TL_DATUM);
212|    canvas.setTextColor(COLOR_FG);
213|}
214|