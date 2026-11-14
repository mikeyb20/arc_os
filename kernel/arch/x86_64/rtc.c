/* arc_os — CMOS real-time clock (M1-06)
 *
 * Owns: reading the date and time from CMOS at boot (ports 0x70/0x71).
 * Invariants: read only; the RTC is never written (no settimeofday yet).
 * Locks: none (BSP, boot, before anything else touches CMOS). */

#include "arch/x86_64/rtc.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/cpuid.h"

#define CMOS_ADDR     0x70
#define CMOS_DATA     0x71
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B
#define CMOS_A_UIP    0x80   /* update in progress: the time registers are changing */
#define RTC_HOUR_PM   0x80   /* 12-hour clock: afternoon */

static const uint8_t rtc_regs[RTC_RAW_COUNT - 1] = {0x00, 0x02, 0x04, 0x07, 0x08, 0x09};

static uint8_t bcd(uint8_t v) {
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

int rtc_decode(const uint8_t raw[RTC_RAW_COUNT], uint8_t status_b, RtcTime *out) {
    int binary = (status_b & RTC_B_BINARY) != 0;
    uint8_t v[RTC_RAW_COUNT];
    for (int i = 0; i < RTC_RAW_COUNT; i++) {
        uint8_t r = raw[i];
        if (i == RTC_RAW_HOURS) r &= (uint8_t)~RTC_HOUR_PM;   /* the PM bit is not part of the number */
        v[i] = binary ? r : bcd(r);
    }
    uint8_t hour = v[RTC_RAW_HOURS];
    if (!(status_b & RTC_B_24H)) {   /* 12-hour clock: 12 AM is 0, 12 PM is 12 */
        if (hour == 12) hour = 0;
        if (raw[RTC_RAW_HOURS] & RTC_HOUR_PM) hour = (uint8_t)(hour + 12);
    }
    uint8_t yy = v[RTC_RAW_YEAR];
    uint16_t year;
    if (raw[RTC_RAW_CENTURY] != 0 && v[RTC_RAW_CENTURY] >= 19 && v[RTC_RAW_CENTURY] <= 99)
        year = (uint16_t)(v[RTC_RAW_CENTURY] * 100 + yy);
    else
        year = (uint16_t)(yy + (yy >= 70 ? 1900 : 2000));
    out->year = year;
    out->month = v[RTC_RAW_MONTH];
    out->day = v[RTC_RAW_DAY];
    out->hour = hour;
    out->minute = v[RTC_RAW_MINUTES];
    out->second = v[RTC_RAW_SECONDS];
    if (yy > 99 || out->month < 1 || out->month > 12 || out->day < 1 || out->day > 31 || out->hour > 23 ||
        out->minute > 59 || out->second > 59)
        return -1;
    return 0;
}

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static void rtc_read_raw(uint8_t century_reg, uint8_t raw[RTC_RAW_COUNT]) {
    for (int i = 0; i < RTC_RAW_COUNT - 1; i++) raw[i] = cmos_read(rtc_regs[i]);
    raw[RTC_RAW_CENTURY] = century_reg != 0 ? cmos_read(century_reg) : 0;
}

int rtc_read(uint8_t century_reg, uint64_t max_cycles, RtcTime *out) {
    uint8_t a[RTC_RAW_COUNT], b[RTC_RAW_COUNT];
    uint64_t start = cpu_rdtsc();
    for (;;) {
        /* An update takes under 2 ms once a second; read twice around none */
        while (cmos_read(CMOS_STATUS_A) & CMOS_A_UIP) {
            if (cpu_rdtsc() - start > max_cycles) return -1;
        }
        rtc_read_raw(century_reg, a);
        while (cmos_read(CMOS_STATUS_A) & CMOS_A_UIP) {
            if (cpu_rdtsc() - start > max_cycles) return -1;
        }
        rtc_read_raw(century_reg, b);
        int same = 1;
        for (int i = 0; i < RTC_RAW_COUNT; i++) same &= a[i] == b[i];
        if (same) break;
        if (cpu_rdtsc() - start > max_cycles) return -1;
    }
    return rtc_decode(b, cmos_read(CMOS_STATUS_B), out);
}
