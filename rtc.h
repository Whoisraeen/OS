#ifndef RTC_H
#define RTC_H

#include <stdint.h>

// Time structure
typedef struct {
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day;
    uint8_t month;
    uint16_t year;
    uint8_t weekday;
} rtc_time_t;

// Initialize RTC
void rtc_init(void);

// Read current time from CMOS RTC
void rtc_get_time(rtc_time_t *time);

// Get Unix-like timestamp (seconds since 2000-01-01 00:00:00)
uint64_t rtc_get_timestamp(void);

#endif
