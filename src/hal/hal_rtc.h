#pragma once
#include <cstdint>

struct hal_rtc_datetime_t {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

void hal_rtc_init();
void hal_rtc_getDateTime(hal_rtc_datetime_t* dt);
uint32_t hal_rtc_getUnixTime();
