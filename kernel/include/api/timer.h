#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include "rtctime.h"

// TWO RATES, AND THEY ARE DIFFERENT THINGS.
//
// CONFIG_HZ (build.conf's `option hz`, build/gen/kconfig.h) is how
// often the TICK interrupts a busy CPU. PIT_HZ is the rate pit_ticks()
// counts at, and it is fixed at 100 -- a coarse monotonic clock for
// timeouts measured in tens of milliseconds, and SYS_TICKS' USER_HZ.
// Anything finer than that reads clocksource_now_ns(), never a tick.
// (The name predates the LAPIC timer; pit_ticks() is not the PIT's.)
#include "kconfig.h"
#define PIT_HZ 100   // == USER_HZ; proc_syscalls.c asserts it
#define TICK_NS (1000000000ull / CONFIG_HZ)

void pit_init(uint32_t frequency_hz);
// Advances the tick counter pit_ticks() returns, and drains the queued
// kernel log. Called from clockevent_tick(), NOT from an interrupt
// handler directly -- which device is interrupting is the clockevent's
// business (kernel/clockevent.h), and by the time this runs it may not
// be the PIT at all.
void timer_tick_advance(void);

// PIT_HZ ticks since boot. Derived from the clocksource when that runs
// without interrupts (clocksource_deadline_capable()), so it advances
// through a stopped tick and inside a syscall; counted from the tick
// interrupt otherwise, where it stands still with IF clear.
uint64_t pit_ticks(void);

// Tick INTERRUPTS delivered, at CONFIG_HZ while the tick runs. What the
// PIT clocksource reads, and what a test counts to see the tick stop.
uint64_t timer_irq_ticks(void);

// `struct rtc_time` is abi/rtctime.h: the TYPE crosses into ring 3, the
// functions below do not.

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
