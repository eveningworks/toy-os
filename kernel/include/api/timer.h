#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

// The rate the system timer actually runs at, so code converting
// between ticks and real time says so instead of repeating 100. It was
// a bare literal at the pit_init() call and a "100 Hz" remark in two
// other files' comments -- fine until something had to turn a client's
// milliseconds into ticks (TWP's WIN_REQ_TIMER) and would have hardcoded
// it a fourth time, in a place where being wrong just makes every
// interval silently the wrong length.
#define PIT_HZ 100

void pit_init(uint32_t frequency_hz);
void pit_handle_irq(void);
uint64_t pit_ticks(void);

struct rtc_time {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t day;
    uint8_t month;
    uint16_t year;
};

void rtc_read(struct rtc_time *t);

#endif
