#pragma once

void hal_audio_init();
void hal_audio_play(uint16_t frequency, uint32_t duration_ms);
void hal_audio_stop();
void hal_audio_beep();
void hal_audio_click();
