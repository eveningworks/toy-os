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
