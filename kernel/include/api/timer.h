#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

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
