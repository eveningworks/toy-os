#include "timer.h"
#include "io.h"
#include "serial.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_FREQ 1193182

static volatile uint64_t ticks = 0;

void pit_init(uint32_t frequency_hz) {
    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;
    outb(PIT_COMMAND, 0x36); // channel 0, lobyte/hibyte, mode 3 (square wave)
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}

void pit_handle_irq(void) {
    ticks++;
    serial_tx_poll(); // a queued log line still moves with nothing printing
}

uint64_t pit_ticks(void) {
    return ticks;
}

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int cmos_update_in_progress(void) {
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (v & 0x0F) + ((v / 16) * 10);
}

void rtc_read(struct rtc_time *t) {
    while (cmos_update_in_progress());

    uint8_t second = cmos_read(0x00);
    uint8_t minute = cmos_read(0x02);
    uint8_t hour   = cmos_read(0x04);
    uint8_t day    = cmos_read(0x07);
    uint8_t month  = cmos_read(0x08);
    uint8_t year   = cmos_read(0x09);

    uint8_t reg_b = cmos_read(0x0B);

    if (!(reg_b & 0x04)) { // BCD mode
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour   = bcd_to_bin(hour & 0x7F) | (hour & 0x80);
        day    = bcd_to_bin(day);
        month  = bcd_to_bin(month);
        year   = bcd_to_bin(year);
    }

    if (!(reg_b & 0x02) && (hour & 0x80)) { // 12-hour mode with PM flag
        hour = ((hour & 0x7F) + 12) % 24;
    }

    t->second = second;
    t->minute = minute;
    t->hour = hour;
    t->day = day;
    t->month = month;
    t->year = 2000 + year; // assumes 21st century
}

static uint8_t bin_to_bcd(uint8_t v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static void cmos_write(uint8_t reg, uint8_t value) {
    outb(CMOS_ADDR, reg);
    outb(CMOS_DATA, value);
}

int rtc_write(const struct rtc_time *t) {
    if (!t || t->year < 1970 || t->year > 2099 || t->month < 1 || t->month > 12 ||
        t->day < 1 || t->day > 31 || t->hour > 23 || t->minute > 59 || t->second > 59) {
        return 0;
    }

    uint8_t reg_b = cmos_read(0x0B);

    // FREEZE THE UPDATE CYCLE FIRST. With SET clear, the RTC increments
    // its own registers while these six writes are in flight, so a write
    // landing on the wrong side of a second boundary is silently undone.
    cmos_write(0x0B, (uint8_t)(reg_b | 0x80));

    uint8_t hour = t->hour;
    // 12-HOUR MODE IS READ BACK, NEVER IMPOSED. A machine whose firmware
    // set the RTC to 12-hour keeps it: switching it to 24-hour here
    // would be correct for this OS and wrong for whatever else boots on
    // the same hardware, which is not a trade a clock write gets to make.
    if (!(reg_b & 0x02)) {
        uint8_t pm = hour >= 12 ? 0x80 : 0;
        uint8_t h12 = (uint8_t)(hour % 12);
        if (h12 == 0) h12 = 12;
        hour = (uint8_t)(h12 | pm);
    }

    uint8_t second = t->second, minute = t->minute;
    uint8_t day = t->day, month = t->month;
    uint8_t year = (uint8_t)(t->year % 100);

    if (!(reg_b & 0x04)) { // BCD mode -- the same test rtc_read() makes
        second = bin_to_bcd(second);
        minute = bin_to_bcd(minute);
        // The PM flag rides ABOVE the digits, so it is preserved across
        // the conversion rather than being encoded as part of the hour.
        hour = (uint8_t)(bin_to_bcd((uint8_t)(hour & 0x7F)) | (hour & 0x80));
        day = bin_to_bcd(day);
        month = bin_to_bcd(month);
        year = bin_to_bcd(year);
    }

    cmos_write(0x00, second);
    cmos_write(0x02, minute);
    cmos_write(0x04, hour);
    cmos_write(0x07, day);
    cmos_write(0x08, month);
    cmos_write(0x09, year);

    // Thaw. The RTC resumes counting from what was just written.
    cmos_write(0x0B, reg_b);

    // WHAT IS NOT WRITTEN: the century register (0x32 on most chipsets,
    // and the FADT names it on some). rtc_read() assumes 2000 + year, so
    // writing a century the reader never consults would be state only
    // another OS could see.
    return 1;
}
