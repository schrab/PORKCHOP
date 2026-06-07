#include "hal_audio.h"
#include "hal_pins.h"
#include <Arduino.h>
#include "driver/ledc.h"

// LEDC config for passive piezo buzzer
// Piezo needs a square wave at the desired frequency.
// ledcWriteTone handles this: sets up a 50% duty PWM at the frequency.

static bool audio_initialized = false;

void hal_audio_init() {
    if (audio_initialized) return;

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = PIEZO_LEDC_RESOLUTION,
        .timer_num = PIEZO_LEDC_TIMER,
        .freq_hz = 2000,      // default 2kHz
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .gpio_num = PIN_PIEZO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = PIEZO_LEDC_CHANNEL,
        .timer_sel = PIEZO_LEDC_TIMER,
        .duty = 0,            // off initially
        .hpoint = 0
    };
    ledc_channel_config(&channel);

    audio_initialized = true;
}

void hal_audio_play(uint16_t frequency, uint32_t duration_ms) {
    if (!audio_initialized) return;
    // 50% duty for audible square wave
    uint32_t duty = (1 << PIEZO_LEDC_RESOLUTION) / 2;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, PIEZO_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, PIEZO_LEDC_CHANNEL);
    ledc_set_freq(LEDC_LOW_SPEED_MODE, PIEZO_LEDC_TIMER, frequency);
    // Non-blocking: SFX engine manages note duration and calls hal_audio_stop()
    (void)duration_ms;
}

void hal_audio_stop() {
    if (!audio_initialized) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, PIEZO_LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, PIEZO_LEDC_CHANNEL);
}

void hal_audio_beep() {
    hal_audio_play(2000, 100);
}

void hal_audio_click() {
    hal_audio_play(1000, 20);
}
