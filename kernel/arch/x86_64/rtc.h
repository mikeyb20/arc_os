#ifndef ARCHOS_ARCH_X86_64_RTC_H
#define ARCHOS_ARCH_X86_64_RTC_H

#include <stdint.h>

/* CMOS real-time clock (M1-06): read once at boot for the wall clock. The
 * RTC is taken to hold UTC. */

typedef struct {
    uint16_t year;    /* full year, e.g. 2026 */
    uint8_t month;    /* 1-12 */
    uint8_t day;      /* 1-31 */
    uint8_t hour;     /* 0-23 */
    uint8_t minute;   /* 0-59 */
    uint8_t second;   /* 0-59 */
} RtcTime;

/* The CMOS registers rtc_decode reads, in this order */
#define RTC_RAW_SECONDS 0
#define RTC_RAW_MINUTES 1
#define RTC_RAW_HOURS   2
#define RTC_RAW_DAY     3
#define RTC_RAW_MONTH   4
#define RTC_RAW_YEAR    5
#define RTC_RAW_CENTURY 6   /* 0 when ACPI names no century register */
#define RTC_RAW_COUNT   7

/* Status register B bits */
#define RTC_B_24H    0x02
#define RTC_B_BINARY 0x04

/* Turn the raw registers into a date: BCD or binary (status B), 12- or
 * 24-hour clock, the century from its register or else 19xx/20xx by the
 * two-digit year (70-99 -> 19xx). Returns 0, or -1 for an impossible date. */
int rtc_decode(const uint8_t raw[RTC_RAW_COUNT], uint8_t status_b, RtcTime *out);

/* Read the RTC: waits out an update in progress and reads until two reads
 * agree, at most max_cycles TSC cycles. century_reg is the FADT's CMOS
 * century index (0 = none). Returns 0, or -1 (no answer or an impossible date). */
int rtc_read(uint8_t century_reg, uint64_t max_cycles, RtcTime *out);

#endif /* ARCHOS_ARCH_X86_64_RTC_H */
