1|// Weather effects module - clouds, rain, thunder, wind
2|// Mood-tied weather system
3|#pragma once
4|
5|6|
7|namespace Weather {
8|
9|// === INITIALIZATION ===
10|void init();
11|
12|// === WEATHER STATE CONTROL ===
13|// Call from Mood system to set weather based on momentum
14|void setMoodLevel(int momentum);  // -100 to 100, affects rain/storm probability
15|
16|// Manual overrides (for testing or special events)
17|void setRaining(bool active);
18|void triggerThunderStorm();
19|
20|// === ANIMATION UPDATES ===
21|// Call each frame to update weather effects
22|void update();
23|
24|// === DRAWING ===
25|// Draw all weather layers (clouds, rain, wind particles)
26|// Call after Avatar::draw() to overlay effects
27|void draw(M5Canvas& canvas, uint16_t colorFG, uint16_t colorBG);
28|
29|// Draw just clouds (parallax layer, call before avatar if desired)
30|void drawClouds(M5Canvas& canvas, uint16_t colorFG);
31|
32|// === THUNDER FLASH ===
33|// Query for thunder flash state (affects screen colors)
34|bool isThunderFlashing();
35|bool isRaining();
36|
37|}  // namespace Weather
38|