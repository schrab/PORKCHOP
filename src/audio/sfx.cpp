1|/**
2| * SFX - Non-blocking Sound Effects Implementation for Porkchop
3| *
4| * ==[ CHEF'S AUDIO ENGINE ]== 
5| * - Note sequences: {freq, duration, pause} steps
6| * - update() ticks without blocking
7| * - Ring buffer for callback-safe event queuing
8| * 
9| * Adapted from Sirloin audio system.
10| */
11|
12|#include "sfx.h"
13|#include "../core/config.h"
14|#include "../hal/hal_audio.h"
15|#include <freertos/FreeRTOS.h>
16|#include <freertos/task.h>
17|
18|namespace SFX {
19|
20|// ==[ SOUND DEFINITIONS ]== arrays of {freq, duration, pause}
21|// freq=0 means silence, duration=0 means END of sequence
22|//
23|// ==[ OPTION D: HYBRID PAPA PIG ]==
24|// Clean terminal sounds for frequent events, pig personality for celebrations.
25|// Professional but warm. The tool of a seasoned hacker pig.
26|//
27|struct Note {
28|    uint16_t freq;      // Hz (0 = silence)
29|    uint16_t duration;  // ms
30|    uint16_t pause;     // ms after this note
31|};
32|
33|// CLICK: Soft mechanical switch tick
34|static const Note SND_CLICK[] = {
35|    {1050, 6, 0},
36|    {0, 0, 0}
37|};
38|
39|// MENU_CLICK: Slightly lower, cushioned click
40|static const Note SND_MENU_CLICK[] = {
41|    {900, 7, 0},
42|    {0, 0, 0}
43|};
44|
45|// TERMINAL_TICK: Mother-style hum pulse (deterministic round-robin)
46|static const Note SND_TERM_TICK_A[] = {
47|    {260, 12, 2},
48|    {540, 3, 0},
49|    {0, 0, 0}
50|};
51|static const Note SND_TERM_TICK_B[] = {
52|    {240, 13, 2},
53|    {500, 3, 0},
54|    {0, 0, 0}
55|};
56|static const Note SND_TERM_TICK_C[] = {
57|    {280, 11, 2},
58|    {600, 3, 0},
59|    {0, 0, 0}
60|};
61|static const Note SND_TERM_TICK_D[] = {
62|    {220, 14, 2},
63|    {460, 3, 2},
64|    {140, 10, 0},
65|    {0, 0, 0}
66|};
67|static const Note SND_TERM_TICK_E[] = {
68|    {300, 10, 2},
69|    {620, 3, 2},
70|    {160, 12, 0},
71|    {0, 0, 0}
72|};
73|
74|// NETWORK_NEW: Short, quiet ping (fires often)
75|static const Note SND_NETWORK[] = {
76|    {820, 5, 0},
77|    {0, 0, 0}
78|};
79|
80|// CLIENT_FOUND: Slightly brighter than network, still short
81|static const Note SND_CLIENT_FOUND[] = {
82|    {1000, 6, 0},
83|    {0, 0, 0}
84|};
85|
86|// DEAUTH: Low punch - impactful, visceral "kick"
87|static const Note SND_DEAUTH[] = {
88|    {400, 70, 0},
89|    {0, 0, 0}
90|};
91|
92|// PMKID: "Truffle found" - pig's ears perk up, quick ascending pair
93|static const Note SND_PMKID[] = {
94|    {1000, 50, 15},
95|    {1300, 50, 0},
96|    {0, 0, 0}
97|};
98|
99|// HANDSHAKE: "Got 'em" - complete phrase with warm resolution
100|// 800→1000→1200 then resolve back to 1000 (closure)
101|static const Note SND_HANDSHAKE[] = {
102|    {800, 60, 15},
103|    {1000, 60, 15},
104|    {1200, 80, 15},
105|    {1000, 100, 0},  // Resolve - the satisfying "done"
106|    {0, 0, 0}
107|};
108|
109|// ACHIEVEMENT: "Papa proud" - warm, earned feeling
110|static const Note SND_ACHIEVEMENT[] = {
111|    {600, 80, 25},
112|    {900, 80, 25},
113|    {1200, 100, 0},
114|    {0, 0, 0}
115|};
116|
117|// LEVEL_UP: "Oink of glory" - ascending major, proper celebration
118|static const Note SND_LEVEL_UP[] = {
119|    {500, 80, 20},
120|    {700, 80, 20},
121|    {1000, 80, 20},
122|    {1200, 120, 0},
123|    {0, 0, 0}
124|};
125|
126|// JACKPOT_XP: Exciting but not annoying - quick rising phrase
127|static const Note SND_JACKPOT[] = {
128|    {700, 50, 15},
129|    {900, 50, 15},
130|    {1100, 50, 15},
131|    {1400, 100, 0},
132|    {0, 0, 0}
133|};
134|
135|// ULTRA_STREAK: Big moment - extended celebration
136|static const Note SND_ULTRA_STREAK[] = {
137|    {500, 60, 15},
138|    {700, 60, 15},
139|    {900, 60, 15},
140|    {1100, 80, 20},
141|    {1400, 150, 0},
142|    {0, 0, 0}
143|};
144|
145|// CALL_RING: Phone pip - attention getter
146|static const Note SND_RING[] = {
147|    {900, 80, 40},
148|    {1100, 80, 0},
149|    {0, 0, 0}
150|};
151|
152|// SYNC_COMPLETE: Success - clean resolution
153|static const Note SND_SYNC_COMPLETE[] = {
154|    {800, 70, 20},
155|    {1000, 70, 20},
156|    {1200, 100, 0},
157|    {0, 0, 0}
158|};
159|
160|// ERROR: Soft low double tap
161|static const Note SND_ERROR[] = {
162|    {240, 50, 20},
163|    {180, 60, 0},
164|    {0, 0, 0}
165|};
166|
167|// BOOT: Nostromo-style long boot sequence (2-3s)
168|static const Note SND_BOOT[] = {
169|    {140, 650, 140},  // low hum pulse
170|    {600, 12, 30},
171|    {700, 12, 30},
172|    {520, 12, 60},
173|    {120, 180, 80},  // tape thud
174|    {800, 12, 30},
175|    {640, 12, 30},
176|    {500, 12, 60},
177|    {900, 10, 30},
178|    {700, 10, 30},
179|    {850, 10, 60},
180|    {170, 230, 70},  // tape thud
181|    {210, 320, 90},
182|    {240, 360, 0},
183|    {0, 0, 0}
184|};
185|
186|// PIGSYNC_BOOT: Shorter wake sequence for FA/TH/ER
187|static const Note SND_PIGSYNC_BOOT[] = {
188|    {160, 480, 140},
189|    {540, 12, 40},
190|    {660, 12, 40},
191|    {560, 12, 80},
192|    {120, 160, 70},  // tape thud
193|    {820, 10, 40},
194|    {700, 10, 60},
195|    {190, 210, 70},
196|    {220, 220, 70},
197|    {180, 240, 0},
198|    {0, 0, 0}
199|};
200|
201|// SIREN: Quick alternating for visual effect sync
202|static const Note SND_SIREN[] = {
203|    {500, 35, 0},
204|    {800, 35, 0},
205|    {500, 35, 0},
206|    {800, 35, 0},
207|    {0, 0, 0}
208|};
209|
210|// ==[ SPECTRUM MODE SOUNDS ]==
211|
212|// SIGNAL_LOST: "Gone" - sad descending
213|static const Note SND_SIGNAL_LOST[] = {
214|    {800, 80, 25},
215|    {500, 120, 0},
216|    {0, 0, 0}
217|};
218|
219|// CHANNEL_LOCK: Quick confirmation tick
220|static const Note SND_CHANNEL_LOCK[] = {
221|    {900, 40, 0},
222|    {0, 0, 0}
223|};
224|
225|// REVEAL_START: Ascending pair - "searching"
226|static const Note SND_REVEAL_START[] = {
227|    {700, 40, 15},
228|    {1000, 50, 0},
229|    {0, 0, 0}
230|};
231|
232|// ==[ CHALLENGE SOUNDS ]==
233|
234|// CHALLENGE_COMPLETE: "Nice work" - similar to achievement, lighter
235|static const Note SND_CHALLENGE_COMPLETE[] = {
236|    {700, 60, 20},
237|    {900, 60, 20},
238|    {1100, 80, 0},
239|    {0, 0, 0}
240|};
241|
242|// CHALLENGE_SWEEP: "Legendary" - the big one with resolve
243|static const Note SND_CHALLENGE_SWEEP[] = {
244|    {800, 70, 20},
245|    {1000, 70, 20},
246|    {1200, 70, 20},
247|    {1500, 100, 15},
248|    {1200, 80, 0},  // Resolve down - closure
249|    {0, 0, 0}
250|};
251|
252|// YOU_DIED: "Dark Souls" style death sound
253|// Impact (43Hz), then F3 wobble (172/178), with dissonant B3/Eb4, fading to sub
254|static const Note SND_YOU_DIED[] = {
255|    {43, 200, 20},   // Impact F1 (Sub-bass thud)
256|    {172, 80, 0},    // F3 wobble 1
257|    {178, 80, 0},    // F3 wobble 1
258|    {172, 80, 0},    // F3 wobble 2
259|    {178, 80, 0},    // F3 wobble 2
260|    {247, 60, 0},    // B3 (poison/dissonance)
261|    {172, 80, 0},    // F3 wobble 3
262|    {178, 80, 0},    // F3 wobble 3
263|    {311, 60, 0},    // Eb4 (metallic edge)
264|    {174, 400, 0},   // F3 sustain (The "Doom Tone")
265|    {87, 400, 0},    // F2 drop
266|    {43, 800, 0},    // F1 - tail (Sub-bass fade)
267|    {0, 0, 0}
268|};
269|
270|// ==[ MORSE REMOVED ]==
271|// Morse GG was too long (600ms+), replaced with warm resolve in HANDSHAKE
272|
273|// ==[ STATE MACHINE ]==
274|static const Note* currentSequence = nullptr;
275|static uint8_t currentStep = 0;
276|static uint32_t stepStartTime = 0;
277|static bool inNote = false;  // true = playing tone, false = in pause
278|
279|// ==[ EVENT RING BUFFER ]== prevents event loss under rapid fire
280|static constexpr uint8_t QUEUE_SIZE = 4;
281|static Event eventQueue[QUEUE_SIZE];
282|static volatile uint8_t queueHead = 0;  // next write position
283|static volatile uint8_t queueTail = 0;  // next read position
284|static portMUX_TYPE queueMutex = portMUX_INITIALIZER_UNLOCKED;
285|
286|// ==[ IMPLEMENTATION ]==
287|
288|void init() {
289|    currentSequence = nullptr;
290|    currentStep = 0;
291|    queueHead = 0;
292|    queueTail = 0;
293|    
294|    // Initialize queue
295|    for (int i = 0; i < QUEUE_SIZE; i++) {
296|        eventQueue[i] = NONE;
297|    }
298|}
299|
300|void play(Event event) {
301|    if (!Config::personality().soundEnabled) return;
302|    if (event == NONE) return;
303|    
304|    // Priority events (captures/celebrations) interrupt anything else
305|    bool isPriority = (event == PMKID || event == HANDSHAKE || event == ACHIEVEMENT || 
306|                       event == LEVEL_UP || event == JACKPOT_XP || event == ULTRA_STREAK ||
307|                       event == CHALLENGE_SWEEP);
308|    if (isPriority && currentSequence != nullptr) {
309|        // Interrupt current sound for priority feedback
310|        hal_audio_stop();
311|        delayMicroseconds(100);  // Brief settle time for audio driver stability
312|        currentSequence = nullptr;
313|        currentStep = 0;
314|        // Clear queue on priority
315|        taskENTER_CRITICAL(&queueMutex);
316|        queueHead = queueTail = 0;
317|        taskEXIT_CRITICAL(&queueMutex);
318|    }
319|    
320|    // Enqueue event (ring buffer - drops oldest if full)
321|    taskENTER_CRITICAL(&queueMutex);
322|    uint8_t nextHead = (queueHead + 1) % QUEUE_SIZE;
323|    if (nextHead == queueTail) {
324|        // Buffer full - advance tail (drop oldest)
325|        queueTail = (queueTail + 1) % QUEUE_SIZE;
326|    }
327|    eventQueue[queueHead] = event;
328|    queueHead = nextHead;
329|    taskEXIT_CRITICAL(&queueMutex);
330|}
331|
332|static void startSequence(const Note* seq) {
333|    currentSequence = seq;
334|    currentStep = 0;
335|    stepStartTime = millis();
336|    inNote = true;
337|    
338|    // Start first note
339|    if (seq[0].freq > 0 && seq[0].duration > 0) {
340|        hal_audio_play(seq[0].freq, seq[0].duration);
341|    }
342|}
343|
344|bool update() {
345|    // Skip if sound disabled
346|    if (!Config::personality().soundEnabled) {
347|        // Clear any queued events
348|        taskENTER_CRITICAL(&queueMutex);
349|        queueHead = queueTail;
350|        taskEXIT_CRITICAL(&queueMutex);
351|        currentSequence = nullptr;
352|        return false;
353|    }
354|    
355|    // Process queued event if nothing playing
356|    taskENTER_CRITICAL(&queueMutex);
357|    bool hasEvents = (queueTail != queueHead && currentSequence == nullptr);
358|    Event e = NONE;
359|    if (hasEvents) {
360|        e = eventQueue[queueTail];
361|        queueTail = (queueTail + 1) % QUEUE_SIZE;
362|    }
363|    taskEXIT_CRITICAL(&queueMutex);
364|    
365|    if (hasEvents) {
366|        switch (e) {
367|            case DEAUTH:
368|                startSequence(SND_DEAUTH);
369|                break;
370|            case HANDSHAKE:
371|                startSequence(SND_HANDSHAKE);
372|                break;
373|            case PMKID:
374|                startSequence(SND_PMKID);
375|                break;
376|            case NETWORK_NEW:
377|                startSequence(SND_NETWORK);
378|                break;
379|            case ACHIEVEMENT:
380|                startSequence(SND_ACHIEVEMENT);
381|                break;
382|            case LEVEL_UP:
383|                startSequence(SND_LEVEL_UP);
384|                break;
385|            case JACKPOT_XP:
386|                startSequence(SND_JACKPOT);
387|                break;
388|            case ULTRA_STREAK:
389|                startSequence(SND_ULTRA_STREAK);
390|                break;
391|            case CALL_RING:
392|                startSequence(SND_RING);
393|                break;
394|            case SYNC_COMPLETE:
395|                startSequence(SND_SYNC_COMPLETE);
396|                break;
397|            case ERROR:
398|                startSequence(SND_ERROR);
399|                break;
400|            case CLICK:
401|                startSequence(SND_CLICK);
402|                break;
403|            case MENU_CLICK:
404|                startSequence(SND_MENU_CLICK);
405|                break;
406|            case TERMINAL_TICK:
407|                {
408|                    static uint8_t termTickIndex = 0;
409|                    const Note* seq = SND_TERM_TICK_A;
410|                    switch (termTickIndex % 5) {
411|                        case 1: seq = SND_TERM_TICK_B; break;
412|                        case 2: seq = SND_TERM_TICK_C; break;
413|                        case 3: seq = SND_TERM_TICK_D; break;
414|                        case 4: seq = SND_TERM_TICK_E; break;
415|                        default: break;
416|                    }
417|                    termTickIndex++;
418|                    startSequence(seq);
419|                }
420|                break;
421|            case BOOT:
422|                startSequence(SND_BOOT);
423|                break;
424|            case PIGSYNC_BOOT:
425|                startSequence(SND_PIGSYNC_BOOT);
426|                break;
427|            case SIREN:
428|                startSequence(SND_SIREN);
429|                break;
430|            case CLIENT_FOUND:
431|                startSequence(SND_CLIENT_FOUND);
432|                break;
433|            case SIGNAL_LOST:
434|                startSequence(SND_SIGNAL_LOST);
435|                break;
436|            case CHANNEL_LOCK:
437|                startSequence(SND_CHANNEL_LOCK);
438|                break;
439|            case REVEAL_START:
440|                startSequence(SND_REVEAL_START);
441|                break;
442|            case CHALLENGE_COMPLETE:
443|                startSequence(SND_CHALLENGE_COMPLETE);
444|                break;
445|            case CHALLENGE_SWEEP:
446|                startSequence(SND_CHALLENGE_SWEEP);
447|                break;
448|            case YOU_DIED:
449|                startSequence(SND_YOU_DIED);
450|                break;
451|            default:
452|                break;
453|        }
454|    }
455|    
456|    // Process current sequence
457|    if (currentSequence == nullptr) {
458|        taskENTER_CRITICAL(&queueMutex);
459|        bool eventsWaiting = (queueTail != queueHead);
460|        taskEXIT_CRITICAL(&queueMutex);
461|        return eventsWaiting;  // More events waiting?
462|    }
463|    
464|    uint32_t now = millis();
465|    const Note& note = currentSequence[currentStep];
466|    
467|    // Check if sequence ended (duration=0 marks end)
468|    if (note.duration == 0) {
469|        currentSequence = nullptr;
470|        currentStep = 0;
471|        taskENTER_CRITICAL(&queueMutex);
472|        bool eventsWaiting = (queueTail != queueHead);
473|        taskEXIT_CRITICAL(&queueMutex);
474|        return eventsWaiting;
475|    }
476|    
477|    if (inNote) {
478|        // In note phase - wait for duration
479|        if (now - stepStartTime >= note.duration) {
480|            // Note finished, enter pause phase
481|            inNote = false;
482|            stepStartTime = now;
483|            
484|            // If no pause, advance immediately
485|            if (note.pause == 0) {
486|                currentStep++;
487|                inNote = true;
488|                stepStartTime = now;
489|                
490|                const Note& next = currentSequence[currentStep];
491|                if (next.duration > 0 && next.freq > 0) {
492|                    hal_audio_play(next.freq, next.duration);
493|                }
494|            }
495|        }
496|    } else {
497|        // In pause phase - wait for pause duration
498|        if (now - stepStartTime >= note.pause) {
499|            // Pause finished, advance to next note
500|            currentStep++;
501|