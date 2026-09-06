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
// Advances the tick counter pit_ticks() returns, and drains the queued
// kernel log. Called from clockevent_tick(), NOT from an interrupt
// handler directly -- which device is interrupting is the clockevent's
// business (kernel/clockevent.h), and by the time this runs it may not
// be the PIT at all.
void timer_tick_advance(void);
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

// Writes `t` back to the CMOS. Returns 1, or 0 if the values are not a
// date the RTC can hold (the caller is expected to have derived them
// from a real clock, so this is a guard, not a validator).
//
// **THE RTC IS NOT THE WALL CLOCK ANY MORE** -- api/ktime.h is, and it
// reads this once at boot. Call ktime_set() to correct the time;
// reaching for this directly moves the hardware without moving the
// clock everything actually reads, and the two then disagree until the
// next reboot.
//
// The write is bracketed by register B's SET bit, which freezes the
// RTC's own update cycle: without it a write landing mid-update is
// discarded or half-applied, and the failure is a clock that is right
// four times out of five.
int rtc_write(const struct rtc_time *t);

#endif
