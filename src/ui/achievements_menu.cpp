1|// Achievements Menu - View unlocked achievements
2|
3|#include "achievements_menu.h"
4|#include "../hal/hal_input.h"
#include "../hal/hal_display.h"
5|#include "display.h"
6|#include "../core/xp.h"
7|#include <ctype.h>
8|#include <string.h>
9|
10|// Static member initialization
11|uint8_t AchievementsMenu::selectedIndex = 0;
12|uint8_t AchievementsMenu::scrollOffset = 0;
13|bool AchievementsMenu::active = false;
14|bool AchievementsMenu::keyWasPressed = false;
15|bool AchievementsMenu::showingDetail = false;
16|
17|// Achievement info - order must match PorkAchievement enum bit positions
18|static const struct {
19|    PorkAchievement flag;
20|    const char* name;
21|    const char* howTo;
22|} ACHIEVEMENTS[] = {
23|    // Original 17 achievements
24|    { ACH_FIRST_BLOOD,    "F1RST BL00D",    "Capture your first handshake" },
25|    { ACH_CENTURION,      "C3NTUR10N",      "Find 100 networks in one session" },
26|    { ACH_MARATHON_PIG,   "MAR4TH0N P1G",   "Walk 10km in a single session" },
27|    { ACH_NIGHT_OWL,      "N1GHT 0WL",      "Hunt after midnight" },
28|    { ACH_GHOST_HUNTER,   "GH0ST HUNT3R",   "Find 10 hidden networks" },
29|    { ACH_APPLE_FARMER,   "4PPLE FARM3R",   "Send 100 Apple BLE packets" },
30|    { ACH_WARDRIVER,      "WARDR1V3R",      "Log 1000 networks lifetime" },
31|    { ACH_DEAUTH_KING,    "D3AUTH K1NG",    "Land 100 successful deauths" },
32|    { ACH_PMKID_HUNTER,   "PMK1D HUNT3R",   "Capture a PMKID" },
33|    { ACH_WPA3_SPOTTER,   "WPA3 SP0TT3R",   "Find a WPA3 network" },
34|    { ACH_GPS_MASTER,     "GPS MAST3R",     "Log 100 GPS-tagged networks" },
35|    { ACH_TOUCH_GRASS,    "T0UCH GR4SS",    "Walk 50km total lifetime" },
36|    { ACH_SILICON_PSYCHO, "S1L1C0N PSYCH0", "Log 5000 networks lifetime" },
37|    { ACH_CLUTCH_CAPTURE, "CLUTCH C4PTUR3", "Handshake at <10% battery" },
38|    { ACH_SPEED_RUN,      "SP33D RUN",      "50 networks in 10 minutes" },
39|    { ACH_CHAOS_AGENT,    "CH40S AG3NT",    "Send 1000 BLE packets" },
40|    { ACH_NIETZSWINE,     "N13TZSCH3",      "Stare into the ether long enough" },
41|    // New 30 achievements
42|    { ACH_TEN_THOUSAND,   "T3N THOU$AND",   "Log 10,000 networks lifetime" },
43|    { ACH_NEWB_SNIFFER,   "N3WB SNIFFER",   "Find your first 10 networks" },
44|    { ACH_FIVE_HUNDRED,   "500 P1GS",       "Find 500 networks in one session" },
45|    { ACH_OPEN_SEASON,    "OPEN S3ASON",    "Find 50 open networks" },
46|    { ACH_WEP_LOLZER,     "WEP L0LZER",     "Find a WEP network (ancient relic)" },
47|    { ACH_HANDSHAKE_HAM,  "HANDSHAK3 HAM",  "Capture 10 handshakes lifetime" },
48|    { ACH_FIFTY_SHAKES,   "F1FTY SHAKES",   "Capture 50 handshakes lifetime" },
49|    { ACH_PMKID_FIEND,    "PMK1D F1END",    "Capture 10 PMKIDs" },
50|    { ACH_TRIPLE_THREAT,  "TR1PLE THREAT",  "Capture 3 handshakes in one session" },
51|    { ACH_HOT_STREAK,     "H0T STREAK",     "Capture 5 handshakes in one session" },
52|    { ACH_FIRST_DEAUTH,   "F1RST D3AUTH",   "Your first successful deauth" },
53|    { ACH_DEAUTH_THOUSAND,"DEAUTH TH0USAND","Land 1000 successful deauths" },
54|    { ACH_RAMPAGE,        "R4MPAGE",        "10 deauths in one session" },
55|    { ACH_HALF_MARATHON,  "HALF MARAT0N",   "Walk 21km in a single session" },
56|    { ACH_HUNDRED_KM,     "HUNDRED K1L0",   "Walk 100km total lifetime" },
57|    { ACH_GPS_ADDICT,     "GPS 4DD1CT",     "Log 500 GPS-tagged networks" },
58|    { ACH_ULTRAMARATHON,  "ULTRAMAR4THON",  "Walk 42.195km in one session" },
59|    { ACH_PARANOID_ANDROID,"PARANOID ANDR01D","Send 100 Android FastPair spam" },
60|    { ACH_SAMSUNG_SPRAY,  "SAMSUNG SPR4Y",  "Send 100 Samsung BLE spam" },
61|    { ACH_WINDOWS_PANIC,  "W1ND0WS PANIC",  "Send 100 Windows SwiftPair spam" },
62|    { ACH_BLE_BOMBER,     "BLE B0MBER",     "Send 5000 BLE packets" },
63|    { ACH_OINKAGEDDON,    "OINK4GEDDON",    "Send 10000 BLE packets" },
64|    { ACH_SESSION_VET,    "SESS10N V3T",    "Complete 100 sessions" },
65|    { ACH_FOUR_HOUR_GRIND,"4 HOUR GR1ND",   "4 hour continuous session" },
66|    { ACH_EARLY_BIRD,     "EARLY B1RD",     "Hunt between 5-7am" },
67|    { ACH_WEEKEND_WARRIOR,"W33KEND WARR10R","Hunt on a weekend" },
68|    { ACH_ROGUE_SPOTTER,  "R0GUE SP0TTER",  "ML detects a rogue AP" },
69|    { ACH_HIDDEN_MASTER,  "H1DDEN MAST3R",  "Find 50 hidden networks" },
70|    { ACH_WPA3_HUNTER,    "WPA3 HUNT3R",    "Find 25 WPA3 networks" },
71|    { ACH_MAX_LEVEL,      "MAX L3VEL",      "Reach level 50" },
72|    { ACH_ABOUT_JUNKIE,   "AB0UT_JUNK13",   "Read the fine print" },
73|    // DO NO HAM achievements (v0.1.4+) - pacifist/stealth playstyle
74|    { ACH_GOING_DARK,     "G01NG D4RK",     "5 minutes in passive mode" },
75|    { ACH_GHOST_PROTOCOL, "GH0ST PR0T0C0L", "30 min passive + 50 networks" },
76|    { ACH_SHADOW_BROKER,  "SH4D0W BR0K3R",  "500 passive networks (unlocks title)" },
77|    { ACH_SILENT_ASSASSIN,"S1L3NT 4SS4SS1N","First passive PMKID capture" },
78|    { ACH_ZEN_MASTER,     "Z3N M4ST3R",     "5 passive PMKIDs (unlocks title)" },
79|    // BOAR BROS achievements (v0.1.4+) - network protection playstyle
80|    { ACH_FIRST_BRO,      "F1RST BR0",      "Add first network to BOAR BROS" },
81|    { ACH_FIVE_FAMILIES,  "F1V3 F4M1L13S",  "5 networks in BOAR BROS" },
82|    { ACH_MERCY_MODE,     "M3RCY M0D3",     "First mid-attack exclusion" },
83|    { ACH_WITNESS_PROTECT,"W1TN3SS PR0T3CT","25 networks protected (unlocks title)" },
84|    { ACH_FULL_ROSTER,    "FULL R0ST3R",    "50 networks in BOAR BROS (max)" },
85|    // Lore achievement (v0.1.8)
86|    { ACH_PROPHECY_WITNESS, "PR0PH3CY W1TN3SS", "Witnessed the riddle prophecy" },
87|    // Combined DO NO HAM + BOAR BROS achievements
88|    { ACH_PACIFIST_RUN,   "P4C1F1ST RUN",   "50+ networks, all added as bros" },
89|    // CLIENT MONITOR achievements (v0.1.6+)
90|    { ACH_QUICK_DRAW,     "QU1CK DR4W",     "Deauth 5 clients in 30 seconds" },
91|    { ACH_DEAD_EYE,       "D34D 3Y3",       "Deauth <2s after entering monitor" },
92|    { ACH_HIGH_NOON,      "H1GH N00N",      "Deauth during noon hour" },
93|    // Ultimate achievement (v0.1.8)
94|    { ACH_FULL_CLEAR,     "TH3_C0MPL3T10N1ST", "Unlock all other achievements" },
95|};
96|
97|void AchievementsMenu::init() {
98|    selectedIndex = 0;
99|    scrollOffset = 0;
100|    showingDetail = false;
101|}
102|
103|void AchievementsMenu::show() {
104|    active = true;
105|    selectedIndex = 0;
106|    scrollOffset = 0;
107|    showingDetail = false;
108|    keyWasPressed = true;  // Ignore the Enter that selected us from menu
109|    updateBottomOverlay();
110|}
111|
112|void AchievementsMenu::hide() {
113|    active = false;
114|    showingDetail = false;
115|    Display::clearBottomOverlay();
116|}
117|
118|void AchievementsMenu::update() {
119|    if (!active) return;
120|    handleInput();
121|}
122|
123|void AchievementsMenu::handleInput() {
124|    bool anyPressed = hal_input_anyHeld();
125|    
126|    if (!anyPressed) {
127|        keyWasPressed = false;
128|        return;
129|    }
130|    
131|    if (keyWasPressed) return;
132|    keyWasPressed = true;
133|    
134|    auto keys = /* keysState replaced */;
135|    
136|    // If showing detail, any key closes it
137|    if (showingDetail) {
138|        showingDetail = false;
139|        return;
140|    }
141|    
142|    // Navigation with ; (up) and . (down)
143|    if (hal_input_wasPressed(KEY_UP)) {
144|        if (selectedIndex > 0) {
145|            selectedIndex--;
146|            if (selectedIndex < scrollOffset) {
147|                scrollOffset = selectedIndex;
148|            }
149|            updateBottomOverlay();
150|        }
151|    }
152|    
153|    if (hal_input_wasPressed(KEY_DOWN)) {
154|        if (selectedIndex < TOTAL_ACHIEVEMENTS - 1) {
155|            selectedIndex++;
156|            if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
157|                scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
158|            }
159|            updateBottomOverlay();
160|        }
161|    }
162|    
163|    // Enter shows detail for selected achievement
164|    if (hal_input_wasPressed(KEY_ENTER)) {
165|        showingDetail = true;
166|        return;
167|    }
168|    
169|    // Backspace - go back
170|    if (hal_input_wasPressed(KEY_BACKSPACE)) {
171|        hide();
172|    }
173|}
174|
175|void AchievementsMenu::draw(M5Canvas& canvas) {
176|    if (!active) return;
177|    
178|    // If showing detail popup, draw that instead
179|    if (showingDetail) {
180|        drawDetail(canvas);
181|        return;
182|    }
183|    
184|    canvas.fillSprite(COLOR_BG);
185|    
186|    // Get unlocked achievements
187|    uint64_t unlocked = XP::getAchievements();
188|    
189|    canvas.setTextColor(COLOR_FG);
190|    canvas.setTextSize(1);
191|    
192|    // Draw achievements list
193|    int y = 2;
194|    int lineHeight = 18;
195|    
196|    for (uint8_t i = scrollOffset; i < TOTAL_ACHIEVEMENTS && i < scrollOffset + VISIBLE_ITEMS; i++) {
197|        bool hasIt = (unlocked & ACHIEVEMENTS[i].flag) != 0;
198|        
199|        // Highlight selected (pink bg, black text) - toast style
200|        if (i == selectedIndex) {
201|            canvas.fillRect(0, y - 1, canvas.width(), lineHeight, COLOR_FG);
202|            canvas.setTextColor(COLOR_BG);
203|        } else {
204|            canvas.setTextColor(COLOR_FG);
205|        }
206|        
207|        // Lock/unlock indicator
208|        canvas.setCursor(4, y);
209|        canvas.print(hasIt ? "[X]" : "[ ]");
210|        
211|        // Achievement name (show ??? if locked)
212|        canvas.setCursor(28, y);
213|        canvas.print(hasIt ? ACHIEVEMENTS[i].name : "???");
214|        
215|        y += lineHeight;
216|    }
217|    
218|    // Scroll indicators
219|    if (scrollOffset > 0) {
220|        canvas.setCursor(canvas.width() - 10, 16);
221|        canvas.setTextColor(COLOR_FG);
222|        canvas.print("^");
223|    }
224|    if (scrollOffset + VISIBLE_ITEMS < TOTAL_ACHIEVEMENTS) {
225|        canvas.setCursor(canvas.width() - 10, 16 + (VISIBLE_ITEMS - 1) * lineHeight);
226|        canvas.setTextColor(COLOR_FG);
227|        canvas.print("v");
228|    }
229|}
230|
231|void AchievementsMenu::drawDetail(M5Canvas& canvas) {
232|    canvas.fillScreen(COLOR_BG);
233|    
234|    bool hasIt = (XP::getAchievements() & ACHIEVEMENTS[selectedIndex].flag) != 0;
235|    
236|    // Toast style: pink filled box with black text
237|    // Taller box to accommodate word-wrapped description
238|    int boxW = 210;
239|    int boxH = 80;
240|    int boxX = (canvas.width() - boxW) / 2;
241|    int boxY = (canvas.height() - boxH) / 2;
242|    
243|    // Black border then pink fill
244|    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
245|    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
246|    
247|    // Black text on pink background
248|    canvas.setTextColor(COLOR_BG, COLOR_FG);
249|    canvas.setTextSize(1);
250|    canvas.setTextDatum(top_center);
251|    
252|    // Achievement name (show UNKNOWN if locked)
253|    canvas.drawString(hasIt ? ACHIEVEMENTS[selectedIndex].name : "UNKNOWN", canvas.width() / 2, boxY + 8);
254|    
255|    // Status
256|    canvas.drawString(hasIt ? "UNLOCKED" : "LOCKED", canvas.width() / 2, boxY + 22);
257|    
258|    // How to get it - with word wrap for long descriptions
259|    const char* howTo = hasIt ? ACHIEVEMENTS[selectedIndex].howTo : "???";
260|    char descBuf[128];
261|    strncpy(descBuf, howTo, sizeof(descBuf) - 1);
262|    descBuf[sizeof(descBuf) - 1] = '\0';
263|    for (size_t i = 0; descBuf[i]; i++) {
264|        descBuf[i] = (char)toupper((unsigned char)descBuf[i]);
265|    }
266|    int maxCharsPerLine = 28;  // Fits ~200px text area
267|    int lineHeight = 12;
268|    int textY = boxY + 40;
269|    int centerX = canvas.width() / 2;
270|    
271|    // Word wrap: split into lines
272|    int lineNum = 0;
273|    const char* cursor = descBuf;
274|    while (*cursor && lineNum < 3) {
275|        size_t remaining = strlen(cursor);
276|        size_t take = remaining <= (size_t)maxCharsPerLine ? remaining : (size_t)maxCharsPerLine;
277|        size_t splitPos = take;
278|        if (remaining > (size_t)maxCharsPerLine) {
279|            for (size_t i = take; i > 0; i--) {
280|                if (cursor[i - 1] == ' ') {
281|                    splitPos = i - 1;
282|                    break;
283|                }
284|            }
285|            if (splitPos == 0) {
286|                splitPos = take;  // Hard break
287|            }
288|        }
289|
290|        char lineBuf[32];
291|        size_t copyLen = splitPos < sizeof(lineBuf) - 1 ? splitPos : sizeof(lineBuf) - 1;
292|        memcpy(lineBuf, cursor, copyLen);
293|        lineBuf[copyLen] = '\0';
294|        canvas.drawString(lineBuf, centerX, textY + lineNum * lineHeight);
295|        lineNum++;
296|
297|        cursor += splitPos;
298|        while (*cursor == ' ') cursor++;
299|    }
300|    
301|    // Reset text datum
302|    canvas.setTextDatum(top_left);
303|}
304|
305|void AchievementsMenu::updateBottomOverlay() {
306|    uint64_t unlocked = XP::getAchievements();
307|    bool hasIt = (unlocked & ACHIEVEMENTS[selectedIndex].flag) != 0;
308|    
309|    if (hasIt) {
310|        // PIG SCREAMS — uppercase copy, truncated to fit 240px bottom bar (~36 chars)
311|        char buf[40];
312|        const char* src = ACHIEVEMENTS[selectedIndex].howTo;
313|        size_t i = 0;
314|        while (src[i] && i < 36) {
315|            buf[i] = (char)toupper((unsigned char)src[i]);
316|            i++;
317|        }
318|        if (src[i]) {  // Was truncated
319|            if (i >= 3) i = 33;
320|            buf[i++] = '.'; buf[i++] = '.'; buf[i++] = '.';
321|        }
322|        buf[i] = '\0';
323|        Display::setBottomOverlay(buf);
324|    } else {
325|        Display::setBottomOverlay("UNKNOWN");
326|    }
327|}
328|