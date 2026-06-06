1|// BOAR BROS Menu - Manage excluded networks
2|
3|#include "boar_bros_menu.h"
4|#include "../hal/hal_input.h"
#include "../hal/hal_display.h"
5|#include <SD.h>
6|#include <ctype.h>
7|#include <string.h>
8|#include "display.h"
9|#include "../modes/oink.h"
10|#include "../core/sd_layout.h"
11|
12|// Static member initialization
13|std::vector<BroInfo> BoarBrosMenu::bros;
14|uint8_t BoarBrosMenu::selectedIndex = 0;
15|uint8_t BoarBrosMenu::scrollOffset = 0;
16|bool BoarBrosMenu::active = false;
17|bool BoarBrosMenu::keyWasPressed = false;
18|bool BoarBrosMenu::deleteConfirmActive = false;
19|
20|void BoarBrosMenu::init() {
21|    bros.clear();
22|    selectedIndex = 0;
23|    scrollOffset = 0;
24|}
25|
26|void BoarBrosMenu::show() {
27|    active = true;
28|    selectedIndex = 0;
29|    scrollOffset = 0;
30|    keyWasPressed = true;  // Ignore the Enter that selected us from menu
31|    deleteConfirmActive = false;
32|    loadBros();
33|}
34|
35|void BoarBrosMenu::hide() {
36|    active = false;
37|    deleteConfirmActive = false;
38|    bros.clear();
39|    bros.shrink_to_fit();  // Release vector memory
40|}
41|
42|void BoarBrosMenu::loadBros() {
43|    bros.clear();
44|    bros.reserve(50);  // Max entries — avoids 6 reallocations during load
45|
46|    const char* boarPath = SDLayout::boarBrosPath();
47|    if (!SD.exists(boarPath)) {
48|        Serial.println("[BOAR_BROS] No file found");
49|        return;
50|    }
51|    
52|    File f = SD.open(boarPath, FILE_READ);
53|    if (!f) {
54|        Serial.println("[BOAR_BROS] Failed to open file");
55|        return;
56|    }
57|    
58|    // Cap at 50 entries (same as MAX_BOAR_BROS in oink.cpp)
59|    // Use stack buffer instead of String to avoid 50 heap alloc/free cycles
60|    char lineBuf[80];  // BSSID(12) + space + SSID(32) + margin
61|    while (f.available() && bros.size() < 50) {
62|        int len = f.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
63|        if (len <= 0) continue;
64|        lineBuf[len] = '\0';
65|
66|        // Trim trailing whitespace
67|        while (len > 0 && (lineBuf[len-1] == '\r' || lineBuf[len-1] == ' ' || lineBuf[len-1] == '\t')) {
68|            lineBuf[--len] = '\0';
69|        }
70|
71|        // Skip leading whitespace
72|        const char* p = lineBuf;
73|        while (*p == ' ' || *p == '\t') p++;
74|        len = strlen(p);
75|
76|        // Skip empty lines and comments
77|        if (len == 0 || *p == '#') continue;
78|
79|        // Format: AABBCCDDEEFF [SSID]
80|        if (len >= 12) {
81|            uint64_t bssid = 0;
82|            bool valid = true;
83|            for (int i = 0; i < 12; i++) {
84|                char c = toupper((unsigned char)p[i]);
85|                uint8_t nibble;
86|                if (c >= '0' && c <= '9') nibble = c - '0';
87|                else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
88|                else { valid = false; break; }
89|                bssid = (bssid << 4) | nibble;
90|            }
91|
92|            if (valid) {
93|                BroInfo info;
94|                memset(&info, 0, sizeof(info));
95|                info.bssid = bssid;
96|                formatBSSID(bssid, info.bssidStr, sizeof(info.bssidStr));
97|
98|                // Extract SSID from rest of line (after space)
99|                if (len > 13) {
100|                    const char* ssid = p + 13;
101|                    while (*ssid == ' ' || *ssid == '\t') ssid++;
102|                    if (*ssid) {
103|                        strncpy(info.ssid, ssid, sizeof(info.ssid) - 1);
104|                    }
105|                }
106|
107|                bros.push_back(info);
108|            }
109|        }
110|    }
111|    
112|    f.close();
113|    Serial.printf("[BOAR_BROS] Loaded %d bros\n", (int)bros.size());
114|}
115|
116|void BoarBrosMenu::formatBSSID(uint64_t bssid, char* out, size_t len) {
117|    if (!out || len == 0) return;
118|    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
119|             (uint8_t)((bssid >> 40) & 0xFF),
120|             (uint8_t)((bssid >> 32) & 0xFF),
121|             (uint8_t)((bssid >> 24) & 0xFF),
122|             (uint8_t)((bssid >> 16) & 0xFF),
123|             (uint8_t)((bssid >> 8) & 0xFF),
124|             (uint8_t)(bssid & 0xFF));
125|}
126|
127|size_t BoarBrosMenu::getCount() {
128|    return OinkMode::getExcludedCount();
129|}
130|
131|void BoarBrosMenu::getSelectedInfo(char* out, size_t len) {
132|    if (!out || len == 0) return;
133|    if (bros.empty()) {
134|        snprintf(out, len, "[B] ADD FROM OINK MODE");
135|        return;
136|    }
137|    if (selectedIndex < bros.size()) {
138|        strncpy(out, bros[selectedIndex].bssidStr, len - 1);
139|        out[len - 1] = '\0';
140|        return;
141|    }
142|    out[0] = '\0';
143|}
144|
145|void BoarBrosMenu::update() {
146|    if (!active) return;
147|    handleInput();
148|}
149|
150|void BoarBrosMenu::handleInput() {
151|    bool anyPressed = hal_input_anyHeld();
152|    
153|    if (!anyPressed) {
154|        keyWasPressed = false;
155|        return;
156|    }
157|    
158|    if (keyWasPressed) return;
159|    keyWasPressed = true;
160|    
161|    auto keys = /* keysState replaced */;
162|    
163|    // Handle delete confirmation modal
164|    if (deleteConfirmActive) {
165|        if (hal_input_wasPressed('y') || hal_input_wasPressed('Y')) {
166|            deleteSelected();
167|            deleteConfirmActive = false;
168|        } else if (hal_input_wasPressed('n') || hal_input_wasPressed('N') ||
169|                   hal_input_wasPressed(KEY_BACKSPACE) || hal_input_wasPressed(KEY_ENTER)) {
170|            deleteConfirmActive = false;  // Cancel
171|        }
172|        return;
173|    }
174|    
175|    // Navigation with ; (prev/up) and . (next/down)
176|    if (hal_input_wasPressed(KEY_UP)) {
177|        if (selectedIndex > 0) {
178|            selectedIndex--;
179|            if (selectedIndex < scrollOffset) {
180|                scrollOffset = selectedIndex;
181|            }
182|        }
183|    }
184|    
185|    if (hal_input_wasPressed(KEY_DOWN)) {
186|        if (!bros.empty() && selectedIndex < bros.size() - 1) {
187|            selectedIndex++;
188|            if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
189|                scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
190|            }
191|        }
192|    }
193|    
194|    // D key - delete selected
195|    if ((hal_input_wasPressed('d') || hal_input_wasPressed('D')) && !bros.empty()) {
196|        deleteConfirmActive = true;
197|    }
198|    
199|    // Backspace - go back
200|    if (hal_input_wasPressed(KEY_BACKSPACE)) {
201|        hide();
202|        // Return to menu handled by porkchop.cpp
203|    }
204|}
205|
206|void BoarBrosMenu::deleteSelected() {
207|    if (selectedIndex >= bros.size()) return;
208|    
209|    uint64_t targetBssid = bros[selectedIndex].bssid;
210|    
211|    // Remove from OinkMode's set and save
212|    OinkMode::removeBoarBro(targetBssid);
213|    
214|    // Refresh our list
215|    loadBros();
216|    
217|    // Adjust selection if needed
218|    if (selectedIndex >= bros.size() && selectedIndex > 0) {
219|        selectedIndex--;
220|    }
221|    if (scrollOffset > 0 && scrollOffset >= bros.size()) {
222|        scrollOffset = bros.size() > 0 ? bros.size() - 1 : 0;
223|    }
224|    
225|    Display::notify(NoticeKind::STATUS, "BRO REMOVED!");
226|}
227|
228|void BoarBrosMenu::draw(M5Canvas& canvas) {
229|    if (!active) return;
230|    
231|    canvas.fillSprite(COLOR_BG);
232|    canvas.setTextColor(COLOR_FG);
233|    canvas.setTextSize(1);
234|    
235|    if (bros.empty()) {
236|        canvas.setCursor(4, 35);
237|        canvas.print("NO BOAR BROS YET!");
238|        canvas.setCursor(4, 50);
239|        canvas.print("PRESS [B] IN OINK MODE");
240|        canvas.setCursor(4, 65);
241|        canvas.print("TO EXCLUDE A NETWORK.");
242|        return;
243|    }
244|    
245|    // Draw bros list
246|    int y = 2;
247|    int lineHeight = 18;
248|    
249|    for (uint8_t i = scrollOffset; i < bros.size() && i < scrollOffset + VISIBLE_ITEMS; i++) {
250|        const BroInfo& bro = bros[i];
251|        
252|        // Highlight selected
253|        if (i == selectedIndex) {
254|            canvas.fillRect(0, y - 1, canvas.width(), lineHeight, COLOR_FG);
255|            canvas.setTextColor(COLOR_BG);
256|        } else {
257|            canvas.setTextColor(COLOR_FG);
258|        }
259|        
260|        // SSID or "NONAME BRO" for hidden networks
261|        canvas.setCursor(4, y);
262|        const char* nameSrc = bro.ssid[0] != '\0' ? bro.ssid : "NONAME BRO";
263|        char displayName[20];
264|        size_t pos = 0;
265|        while (*nameSrc && pos + 1 < sizeof(displayName)) {
266|            displayName[pos++] = (char)toupper((unsigned char)*nameSrc++);
267|        }
268|        displayName[pos] = '\0';
269|        if (pos > 14 && sizeof(displayName) > 14) {
270|            displayName[12] = '.';
271|            displayName[13] = '.';
272|            displayName[14] = '\0';
273|        }
274|        canvas.print(displayName);
275|        
276|        // Full BSSID (fits at x=80, 17 chars * 6px = 102px, ends at 182px)
277|        canvas.setCursor(80, y);
278|        canvas.print(bro.bssidStr);
279|        
280|        y += lineHeight;
281|    }
282|    
283|    // Scroll indicators
284|    if (scrollOffset > 0) {
285|        canvas.setCursor(canvas.width() - 10, 2);
286|        canvas.setTextColor(COLOR_FG);
287|        canvas.print("^");
288|    }
289|    if (scrollOffset + VISIBLE_ITEMS < bros.size()) {
290|        canvas.setCursor(canvas.width() - 10, 2 + (VISIBLE_ITEMS - 1) * lineHeight);
291|        canvas.setTextColor(COLOR_FG);
292|        canvas.print("v");
293|    }
294|    
295|    // Draw delete confirmation modal if active
296|    if (deleteConfirmActive) {
297|        drawDeleteConfirm(canvas);
298|    }
299|}
300|
301|void BoarBrosMenu::drawDeleteConfirm(M5Canvas& canvas) {
302|    // Modal box dimensions - matches other confirmation dialogs
303|    const int boxW = 180;
304|    const int boxH = 55;
305|    const int boxX = (canvas.width() - boxW) / 2;
306|    const int boxY = (canvas.height() - boxH) / 2 - 5;
307|    
308|    // Black border then pink fill
309|    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
310|    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
311|    
312|    // Black text on pink
313|    canvas.setTextColor(COLOR_BG, COLOR_FG);
314|    canvas.setTextDatum(top_center);
315|    
316|    canvas.drawString("REMOVE THIS BRO?", boxX + boxW / 2, boxY + 10);
317|    
318|    const BroInfo& bro = bros[selectedIndex];
319|    const char* broSrc = bro.ssid[0] != '\0' ? bro.ssid : bro.bssidStr;
320|    char broName[24];
321|    size_t broPos = 0;
322|    while (*broSrc && broPos + 1 < sizeof(broName)) {
323|        broName[broPos++] = (char)toupper((unsigned char)*broSrc++);
324|    }
325|    broName[broPos] = '\0';
326|    if (broPos > 18 && sizeof(broName) > 18) {
327|        broName[16] = '.';
328|        broName[17] = '.';
329|        broName[18] = '\0';
330|    }
331|    canvas.drawString(broName, boxX + boxW / 2, boxY + 24);
332|    
333|    canvas.drawString("[Y]ES  [N]O", boxX + boxW / 2, boxY + 40);
334|    
335|    canvas.setTextDatum(top_left);
336|}
337|