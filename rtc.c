#include "rtc.h"
#include "acpi.h"
#include "io.h"
#include "serial.h"

// CMOS ports
#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

// CMOS registers
#define RTC_SECONDS  0x00
#define RTC_MINUTES  0x02
#define RTC_HOURS    0x04
#define RTC_WEEKDAY  0x06
#define RTC_DAY      0x07
#define RTC_MONTH    0x08
#define RTC_YEAR     0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

static uint8_t century_register = 0;

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

static int rtc_is_updating(void) {
    return cmos_read(RTC_STATUS_A) & 0x80;
}

// Convert BCD to binary
static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

void rtc_init(void) {
    acpi_info_t *info = acpi_get_info();
    century_register = info->century_register;
    kprintf("[RTC] Initialized (century_reg=%d)\n", century_register);
}

void rtc_get_time(rtc_time_t *time) {
    // Wait for update to complete
    while (rtc_is_updating());

    uint8_t sec = cmos_read(RTC_SECONDS);
    uint8_t min = cmos_read(RTC_MINUTES);
    uint8_t hr  = cmos_read(RTC_HOURS);
    uint8_t day = cmos_read(RTC_DAY);
    uint8_t mon = cmos_read(RTC_MONTH);
    uint8_t yr  = cmos_read(RTC_YEAR);
    uint8_t wkd = cmos_read(RTC_WEEKDAY);
    uint8_t century = 0;
    if (century_register) {
        century = cmos_read(century_register);
    }

    // Read again to make sure values didn't change during read
    uint8_t sec2, min2, hr2, day2, mon2, yr2;
    do {
        sec2 = sec; min2 = min; hr2 = hr;
        day2 = day; mon2 = mon; yr2 = yr;

        while (rtc_is_updating());
        sec = cmos_read(RTC_SECONDS);
        min = cmos_read(RTC_MINUTES);
        hr  = cmos_read(RTC_HOURS);
        day = cmos_read(RTC_DAY);
        mon = cmos_read(RTC_MONTH);
        yr  = cmos_read(RTC_YEAR);
    } while (sec != sec2 || min != min2 || hr != hr2 ||
             day != day2 || mon != mon2 || yr != yr2);

    // Check if values are BCD
    uint8_t status_b = cmos_read(RTC_STATUS_B);
    int is_binary = status_b & 0x04;
    int is_24h = status_b & 0x02;

    if (!is_binary) {
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        hr  = bcd_to_bin(hr & 0x7F); // Mask PM bit
        day = bcd_to_bin(day);
        mon = bcd_to_bin(mon);
        yr  = bcd_to_bin(yr);
        if (century_register) century = bcd_to_bin(century);
    }

    // Handle 12-hour format
    if (!is_24h && (hr & 0x80)) {
        hr = ((hr & 0x7F) + 12) % 24;
    }

    // Calculate full year
    if (century_register && century) {
        time->year = century * 100 + yr;
    } else {
        time->year = (yr >= 70) ? 1900 + yr : 2000 + yr;
    }

    time->seconds = sec;
    time->minutes = min;
    time->hours = hr;
    time->day = day;
    time->month = mon;
    time->weekday = wkd;
}

uint64_t rtc_get_timestamp(void) {
    rtc_time_t t;
    rtc_get_time(&t);

    // Days per month (non-leap)
    static const uint16_t days_in_month[] = {
        0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    // Calculate days since 2000-01-01
    uint64_t days = 0;
    for (uint16_t y = 2000; y < t.year; y++) {
        days += 365;
        if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)
            days++;
    }
    for (uint8_t m = 1; m < t.month; m++) {
        days += days_in_month[m];
        if (m == 2 && ((t.year % 4 == 0 && t.year % 100 != 0) || t.year % 400 == 0))
            days++;
    }
    days += t.day - 1;

    return days * 86400ULL + t.hours * 3600ULL + t.minutes * 60ULL + t.seconds;
}
