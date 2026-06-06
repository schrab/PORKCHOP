1|// Crash Viewer Menu implementation
2|
3|#include "crash_viewer.h"
4|#include "display.h"
5|#include "../core/config.h"
6|#include "../core/sd_layout.h"
7|#include "../hal/hal_input.h"
#include "../hal/hal_display.h"
8|#include <SD.h>
9|#include <algorithm>
10|#include <time.h>
11|#include <string.h>
12|
13|static void formatDisplayName(const char* path, char* out, size_t len);
14|static void truncateWithEllipsis(char* text, size_t maxLen);
15|static void formatTimeLine(time_t t, char* out, size_t len);
16|
17|static const size_t MAX_CRASH_FILES = 32;
18|
19|bool CrashViewer::active = false;
20|std::vector<CrashViewer::CrashEntry> CrashViewer::crashFiles;
21|std::vector<CrashViewer::LogLine> CrashViewer::fileLines;
22|uint16_t CrashViewer::listScroll = 0;
23|uint16_t CrashViewer::fileScroll = 0;
24|uint16_t CrashViewer::totalLines = 0;
25|uint8_t CrashViewer::selectedIndex = 0;
26|bool CrashViewer::fileViewActive = false;
27|bool CrashViewer::nukeConfirmActive = false;
28|bool CrashViewer::keyWasPressed = false;
29|char CrashViewer::activeFile[64] = {0};
30|
31|static const uint16_t MAX_LOG_LINES = 120;
32|static const uint8_t VISIBLE_LINES = 9;
33|static const uint8_t LINE_HEIGHT = 11;
34|
35|void CrashViewer::init() {
36|    crashFiles.clear();
37|    fileLines.clear();
38|    listScroll = 0;
39|    fileScroll = 0;
40|    totalLines = 0;
41|    selectedIndex = 0;
42|    fileViewActive = false;
43|    nukeConfirmActive = false;
44|    activeFile[0] = '\0';
45|}
46|
47|void CrashViewer::scanCrashFiles() {
48|    crashFiles.clear();
49|    crashFiles.reserve(MAX_CRASH_FILES);  // Cap at 32 — avoids unbounded growth
50|    selectedIndex = 0;
51|    listScroll = 0;
52|
53|    if (!Config::isSDAvailable()) {
54|        return;
55|    }
56|
57|    const char* crashDir = SDLayout::crashDir();
58|    if (!SD.exists(crashDir)) {
59|        return;
60|    }
61|
62|    File dir = SD.open(crashDir);
63|    if (!dir) {
64|        return;
65|    }
66|
67|    uint8_t yieldCounter = 0;
68|    while (crashFiles.size() < MAX_CRASH_FILES) {
69|        File entry = dir.openNextFile();
70|        if (!entry) break;
71|
72|        if (!entry.isDirectory()) {
73|            const char* name = entry.name();
74|            time_t lastWrite = entry.getLastWrite();
75|            entry.close();
76|
77|            // Check .txt extension without String
78|            size_t nameLen = strlen(name);
79|            if (nameLen < 4 || strcmp(name + nameLen - 4, ".txt") != 0) {
80|                continue;
81|            }
82|
83|            // Extract basename without String
84|            const char* base = strrchr(name, '/');
85|            base = base ? base + 1 : name;
86|
87|            CrashEntry entryInfo;
88|            memset(&entryInfo, 0, sizeof(entryInfo));
89|            snprintf(entryInfo.path, sizeof(entryInfo.path), "%s/%s", crashDir, base);
90|            entryInfo.timestamp = lastWrite;
91|            crashFiles.push_back(entryInfo);
92|        } else {
93|            entry.close();
94|        }
95|        
96|        // Yield every 10 files to prevent WDT timeout
97|        if (++yieldCounter >= 10) {
98|            yieldCounter = 0;
99|            yield();
100|        }
101|    }
102|
103|    dir.close();
104|
105|    std::sort(crashFiles.begin(), crashFiles.end(), [](const CrashEntry& a, const CrashEntry& b) {
106|        return a.timestamp > b.timestamp;
107|    });
108|}
109|
110|static void pushLogLine(std::vector<CrashViewer::LogLine>& lines, const char* text) {
111|    CrashViewer::LogLine entry;
112|    strncpy(entry.text, text, sizeof(entry.text) - 1);
113|    entry.text[sizeof(entry.text) - 1] = '\0';
114|    lines.push_back(entry);
115|}
116|
117|void CrashViewer::loadCrashFile(const char* path) {
118|    fileLines.clear();
119|    fileScroll = 0;
120|    totalLines = 0;
121|    strncpy(activeFile, path, sizeof(activeFile) - 1);
122|    activeFile[sizeof(activeFile) - 1] = '\0';
123|
124|    File f = SD.open(path, FILE_READ);
125|    if (!f) {
126|        pushLogLine(fileLines, "FAILED TO OPEN");
127|        char displayName[32];
128|        formatDisplayName(path, displayName, sizeof(displayName));
129|        pushLogLine(fileLines, displayName);
130|        totalLines = fileLines.size();
131|        return;
132|    }
133|
134|    // Read last MAX_LOG_LINES non-empty lines using ring buffer approach
135|    fileLines.reserve(MAX_LOG_LINES);
136|    char lineBuf[80];
137|    uint16_t lineCount = 0;
138|    while (f.available()) {
139|        size_t len = f.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
140|        lineBuf[len] = '\0';
141|        while (len > 0 && (lineBuf[len - 1] == '\r' || lineBuf[len - 1] == ' ')) {
142|            lineBuf[--len] = '\0';
143|        }
144|        if (len == 0) continue;
145|
146|        if (lineCount < MAX_LOG_LINES) {
147|            pushLogLine(fileLines, lineBuf);
148|        } else {
149|            // Shift left by 1 and replace last
150|            for (uint16_t i = 1; i < fileLines.size(); i++) {
151|                fileLines[i - 1] = fileLines[i];
152|            }
153|            strncpy(fileLines.back().text, lineBuf, sizeof(LogLine::text) - 1);
154|            fileLines.back().text[sizeof(LogLine::text) - 1] = '\0';
155|        }
156|        lineCount++;
157|    }
158|    f.close();
159|
160|    totalLines = fileLines.size();
161|
162|    if (fileLines.empty()) {
163|        pushLogLine(fileLines, "EMPTY FILE");
164|        totalLines = 1;
165|    }
166|}
167|
168|void CrashViewer::show() {
169|    active = true;
170|    keyWasPressed = true;
171|    fileViewActive = false;
172|    nukeConfirmActive = false;
173|    activeFile[0] = '\0';
174|    fileLines.clear();
175|    scanCrashFiles();
176|}
177|
178|void CrashViewer::hide() {
179|    active = false;
180|    crashFiles.clear();
181|    fileLines.clear();
182|    crashFiles.shrink_to_fit();
183|    fileLines.shrink_to_fit();
184|    fileViewActive = false;
185|    nukeConfirmActive = false;
186|    activeFile[0] = '\0';
187|    Display::clearBottomOverlay();
188|}
189|
190|void CrashViewer::nukeCrashFiles() {
191|    const char* crashDir = SDLayout::crashDir();
192|    if (!SD.exists(crashDir)) {
193|        return;
194|    }
195|
196|    File dir = SD.open(crashDir);
197|    if (!dir) {
198|        return;
199|    }
200|
201|    uint8_t yieldCounter = 0;
202|    while (true) {
203|        File entry = dir.openNextFile();
204|        if (!entry) break;
205|
206|        if (!entry.isDirectory()) {
207|            const char* name = entry.name();
208|            entry.close();
209|
210|            const char* base = strrchr(name, '/');
211|            base = base ? (base + 1) : name;
212|            char path[80];
213|            snprintf(path, sizeof(path), "%s/%s", crashDir, base);
214|            size_t plen = strlen(path);
215|
216|            if ((plen > 4 && strcmp(path + plen - 4, ".txt") == 0) ||
217|                (plen > 4 && strcmp(path + plen - 4, ".elf") == 0)) {
218|                SD.remove(path);
219|            }
220|        } else {
221|            entry.close();
222|        }
223|        
224|        // Yield every 10 files to prevent WDT timeout
225|        if (++yieldCounter >= 10) {
226|            yieldCounter = 0;
227|            yield();
228|        }
229|    }
230|
231|    dir.close();
232|}
233|
234|void CrashViewer::drawList(M5Canvas& canvas) {
235|    canvas.fillSprite(COLOR_BG);
236|    canvas.setTextColor(COLOR_FG, COLOR_BG);
237|    canvas.setTextSize(1);
238|    canvas.setFont(&fonts::Font0);
239|    canvas.setTextDatum(TL_DATUM);
240|
241|    if (crashFiles.empty()) {
242|        canvas.drawString("NO CRASH FILES", 2, 8);
243|        canvas.drawString("CHECK CRASH DIR", 2, 20);
244|        return;
245|    }
246|
247|    uint16_t count = crashFiles.size();
248|    uint8_t y = 2;
249|    const int timeX = 150;
250|
251|    for (uint8_t i = 0; i < VISIBLE_LINES && (listScroll + i) < count; i++) {
252|        uint8_t idx = listScroll + i;
253|        char displayLine[32];
254|        formatDisplayName(crashFiles[idx].path, displayLine, sizeof(displayLine));
255|        size_t nameLen = strlen(displayLine);
256|        if (nameLen > 22 && sizeof(displayLine) > 22) {
257|            displayLine[21] = '~';
258|            displayLine[22] = '\0';
259|        }
260|        char timeLine[16];
261|        formatTimeLine(crashFiles[idx].timestamp, timeLine, sizeof(timeLine));
262|
263|        bool selected = (idx == selectedIndex);
264|        if (selected) {
265|            canvas.fillRect(0, y - 1, DISPLAY_W, LINE_HEIGHT, COLOR_FG);
266|            canvas.setTextColor(COLOR_BG, COLOR_FG);
267|        } else {
268|            canvas.setTextColor(COLOR_FG, COLOR_BG);
269|        }
270|
271|        canvas.drawString(displayLine, 2, y);
272|        canvas.drawString(timeLine, timeX, y);
273|        y += LINE_HEIGHT;
274|    }
275|
276|    if (count > VISIBLE_LINES) {
277|        int barHeight = MAIN_H - 14;
278|        int barY = 12;
279|        int thumbHeight = max(10, (int)(barHeight * VISIBLE_LINES / count));
280|        int thumbY = barY + (barHeight - thumbHeight) * listScroll / (count - VISIBLE_LINES);
281|
282|        canvas.fillRect(DISPLAY_W - 4, barY, 3, barHeight, COLOR_BG);
283|        canvas.fillRect(DISPLAY_W - 4, thumbY, 3, thumbHeight, COLOR_FG);
284|    }
285|}
286|
287|void CrashViewer::drawFile(M5Canvas& canvas) {
288|    canvas.fillSprite(COLOR_BG);
289|    canvas.setTextColor(COLOR_FG, COLOR_BG);
290|    canvas.setTextSize(1);
291|    canvas.setFont(&fonts::Font0);
292|    canvas.setTextDatum(TL_DATUM);
293|
294|    uint8_t y = 2;
295|
296|    for (uint8_t i = 0; i < VISIBLE_LINES && (fileScroll + i) < totalLines; i++) {
297|        const LogLine& line = fileLines[fileScroll + i];
298|        char displayLine[48];
299|        strncpy(displayLine, line.text, sizeof(displayLine) - 1);
300|        displayLine[sizeof(displayLine) - 1] = '\0';
301|        size_t lineLen = strlen(displayLine);
302|        if (lineLen > 39 && sizeof(displayLine) > 39) {
303|            displayLine[38] = '~';
304|            displayLine[39] = '\0';
305|        }
306|        canvas.drawString(displayLine, 2, y);
307|        y += LINE_HEIGHT;
308|    }
309|
310|    if (totalLines > VISIBLE_LINES) {
311|        int barHeight = MAIN_H - 14;
312|        int barY = 12;
313|        int thumbHeight = max(10, (int)(barHeight * VISIBLE_LINES / totalLines));
314|        int thumbY = barY + (barHeight - thumbHeight) * fileScroll / (totalLines - VISIBLE_LINES);
315|
316|        canvas.fillRect(DISPLAY_W - 4, barY, 3, barHeight, COLOR_BG);
317|        canvas.fillRect(DISPLAY_W - 4, thumbY, 3, thumbHeight, COLOR_FG);
318|    }
319|}
320|
321|void CrashViewer::drawNukeConfirm(M5Canvas& canvas) {
322|    const int boxW = 200;
323|    const int boxH = 70;
324|    const int boxX = (canvas.width() - boxW) / 2;
325|    const int boxY = (canvas.height() - boxH) / 2 - 5;
326|
327|    // Match Captures nuke style
328|    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
329|    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
330|
331|    canvas.setTextColor(COLOR_BG, COLOR_FG);
332|    canvas.setTextDatum(top_center);
333|    canvas.setTextSize(1);
334|
335|    int centerX = canvas.width() / 2;
336|
337|    canvas.drawString("!! SCORCHED EARTH !!", centerX, boxY + 8);
338|    char cmd[48];
339|    snprintf(cmd, sizeof(cmd), "rm -rf %s/*", SDLayout::crashDir());
340|    canvas.drawString(cmd, centerX, boxY + 22);
341|    canvas.drawString("THIS KILLS THE DUMPS.", centerX, boxY + 36);
342|    canvas.drawString("[Y] DO IT  [N] ABORT", centerX, boxY + 54);
343|}
344|
345|void CrashViewer::update() {
346|    if (!active) return;
347|
348|    if (!hal_input_anyHeld()) {
349|        keyWasPressed = false;
350|        return;
351|    }
352|
353|    if (keyWasPressed) return;
354|    keyWasPressed = true;
355|
356|    Keyboard_Class::KeysState keys = /* keysState replaced */;
357|
358|    if (nukeConfirmActive) {
359|        if (hal_input_wasPressed('y') || hal_input_wasPressed('Y')) {
360|            nukeCrashFiles();
361|            nukeConfirmActive = false;
362|            Display::clearBottomOverlay();
363|            fileViewActive = false;
364|            fileLines.clear();
365|            scanCrashFiles();
366|        } else if (hal_input_wasPressed('n') || hal_input_wasPressed('N') ||
367|                   hal_input_wasPressed(KEY_BACKSPACE) || hal_input_wasPressed(KEY_ENTER)) {
368|            nukeConfirmActive = false;
369|            Display::clearBottomOverlay();
370|        }
371|        return;
372|    }
373|
374|    if (fileViewActive) {
375|        if (hal_input_wasPressed(KEY_UP)) {
376|            if (fileScroll > 0) {
377|                fileScroll--;
378|            }
379|        } else if (hal_input_wasPressed(KEY_DOWN)) {
380|            if (totalLines > VISIBLE_LINES && fileScroll < totalLines - VISIBLE_LINES) {
381|                fileScroll++;
382|            }
383|        } else if (hal_input_wasPressed(KEY_BACKSPACE) || hal_input_wasPressed(KEY_ENTER)) {
384|            fileViewActive = false;
385|            fileLines.clear();
386|            totalLines = 0;
387|            activeFile[0] = '\0';
388|        }
389|        return;
390|    }
391|
392|    if (hal_input_wasPressed(KEY_UP)) {
393|        if (selectedIndex > 0) {
394|            selectedIndex--;
395|            if (selectedIndex < listScroll) {
396|                listScroll = selectedIndex;
397|            }
398|        }
399|    } else if (hal_input_wasPressed(KEY_DOWN)) {
400|        if (selectedIndex + 1 < crashFiles.size()) {
401|            selectedIndex++;
402|            if (selectedIndex >= listScroll + VISIBLE_LINES) {
403|                listScroll = selectedIndex - VISIBLE_LINES + 1;
404|            }
405|        }
406|    } else if (hal_input_wasPressed('d') || hal_input_wasPressed('D')) {
407|        if (!crashFiles.empty()) {
408|            nukeConfirmActive = true;
409|            Display::setBottomOverlay("PERMANENT | NO UNDO");
410|        }
411|    } else if (hal_input_wasPressed(KEY_BACKSPACE)) {
412|        hide();
413|    } else if (hal_input_wasPressed(KEY_ENTER)) {
414|        if (!crashFiles.empty() && selectedIndex < crashFiles.size()) {
415|            loadCrashFile(crashFiles[selectedIndex].path);
416|            fileViewActive = true;
417|        }
418|    }
419|}
420|
421|void CrashViewer::draw(M5Canvas& canvas) {
422|    if (!active) return;
423|
424|    if (fileViewActive) {
425|        drawFile(canvas);
426|    } else {
427|        drawList(canvas);
428|    }
429|
430|    if (nukeConfirmActive) {
431|        drawNukeConfirm(canvas);
432|    }
433|}
434|
435|static void formatDisplayName(const char* path, char* out, size_t len) {
436|    if (!out || len == 0) return;
437|    if (!path || path[0] == '\0') {
438|        out[0] = '\0';
439|        return;
440|    }
441|    const char* name = strrchr(path, '/');
442|    name = name ? (name + 1) : path;
443|    size_t nlen = strlen(name);
444|    if (nlen >= len) nlen = len - 1;
445|    memcpy(out, name, nlen);
446|    out[nlen] = '\0';
447|
448|    size_t outLen = strlen(out);
449|    if (outLen >= 4 && strcmp(out + outLen - 4, ".txt") == 0) {
450|        out[outLen - 4] = '\0';
451|    }
452|}
453|
454|static void truncateWithEllipsis(char* text, size_t maxLen) {
455|    if (!text) return;
456|    size_t len = strlen(text);
457|    if (len <= maxLen) return;
458|    if (maxLen < 3) {
459|        text[maxLen] = '\0';
460|        return;
461|    }
462|    size_t cut = maxLen - 3;
463|    text[cut] = '.';
464|    text[cut + 1] = '.';
465|    text[cut + 2] = '.';
466|    text[cut + 3] = '\0';
467|}
468|
469|static void formatTimeLine(time_t t, char* out, size_t len) {
470|    if (!out || len == 0) return;
471|    if (t == 0) {
472|        strncpy(out, "-- -- --:--", len - 1);
473|        out[len - 1] = '\0';
474|        return;
475|    }
476|
477|    struct tm* timeinfo = localtime(&t);
478|    if (!timeinfo) {
479|        strncpy(out, "-- -- --:--", len - 1);
480|        out[len - 1] = '\0';
481|        return;
482|    }
483|
484|    // Format: "Dec 06 14:32"
485|    strftime(out, len, "%b %d %H:%M", timeinfo);
486|}
487|
488|void CrashViewer::getStatusLine(char* out, size_t len) {
489|    if (!out || len == 0) return;
490|    out[0] = '\0';
491|    if (!active) return;
492|
493|    const char* path = nullptr;
494|    if (fileViewActive && activeFile[0] != '\0') {
495|        path = activeFile;
496|    } else if (crashFiles.empty()) {
497|        snprintf(out, len, "NO CRASH FILES");
498|        return;
499|    } else if (selectedIndex < crashFiles.size()) {
500|        path = crashFiles[selectedIndex].path;
501|