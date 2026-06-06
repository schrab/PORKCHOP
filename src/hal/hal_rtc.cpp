#include "hal_rtc.h"
#include <time.h>
#include <sys/time.h>

void hal_rtc_init() {}

void hal_rtc_getDateTime(hal_rtc_datetime_t* dt) {
    if (!dt) return;
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);
    if (tm_info) {
        dt->year = tm_info->tm_year + 1900;
        dt->month = tm_info->tm_mon + 1;
        dt->day = tm_info->tm_mday;
        dt->hour = tm_info->tm_hour;
        dt->minute = tm_info->tm_min;
        dt->second = tm_info->tm_sec;
    }
}

uint32_t hal_rtc_getUnixTime() {
    return (uint32_t)time(nullptr);
}
