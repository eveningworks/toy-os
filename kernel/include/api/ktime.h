#ifndef KTIME_H
#define KTIME_H

#include <stdint.h>
#include "timer.h"

// THE WALL CLOCK, as a software clock anchored to the monotonic
// clocksource -- Linux's timekeeping, minus the parts that need a
// second CPU.
//
// The RTC is read ONCE, at boot, and never again: what a caller gets is
// that reading plus however much monotonic time has passed since. So a
// clock read costs an arithmetic op instead of the ~10 microsecond
// spin on CMOS reg 0x0A that rtc_read() pays, and it advances smoothly
// rather than in whole-second steps.
//
// **THIS CLOCK IS UTC.** The RTC is assumed to hold UTC (the convention
// every Unix follows and Windows does not), and nothing in the kernel
// knows what a timezone is. A caller wanting local civil time converts
// in ring 3 (userland/lib/utz.h), which is where the city database and
// the DST rules live.
//
// WHY THIS IS NOT A `struct clocksource`. clocksource.h refuses a wall
// clock on purpose: a clocksource must be monotonic, and this one jumps
// whenever ktime_set() is called. Wall time is built ON a clocksource,
// which is why this file exists beside it rather than inside it.

// Seeds the clock from the RTC and anchors it to the current
// clocksource reading. Call once, after clocksource_init(). Reading
// before this returns the epoch, which is a wrong answer nobody can
// mistake for a right one.
void ktime_init(void);

// Seconds and nanoseconds since 1970-01-01 00:00:00 UTC.
uint64_t ktime_now_sec(void);
uint64_t ktime_now_ns(void);

// The clock as broken-down UTC civil time -- rtc_read()'s shape, and
// what SYS_GETTIME hands to ring 3.
void ktime_read(struct rtc_time *out);

// Steps the clock to `sec` seconds plus `nsec` nanoseconds since
// 1970-01-01 UTC, and writes the RTC so the correction survives a
// reboot.
//
// **THE NANOSECONDS ARE NOT DECORATION.** This took whole seconds at
// first, which threw away the sub-second part of every correction and
// left the clock up to a second late -- measured on the test laptop as
// a steady few hundred milliseconds behind, immediately after a sync
// that had just reported a 7 ms round trip. A clock built to carry
// sub-second time has to be settable to one.
//
// A STEP, NOT A SLEW. A real NTP implementation adjusts the tick rate
// so time never runs backwards; SNTP steps, this steps, and a caller
// that cares about an interval uses SYS_MONOTONIC_NS, which this cannot
// move. Refuses a year outside 1970..9999, which is the range the RTC
// and the calendar arithmetic can both represent.
//
// Returns 1 on success, 0 if the epoch is out of range. The RTC write
// failing does NOT fail the call: the in-memory clock is corrected
// either way, and a machine whose CMOS is unwritable still wants the
// right time until it reboots.
int ktime_set(uint64_t sec, uint32_t nsec);

// How far the clock has been moved since boot, in seconds, and how many
// times. Both are diagnostics -- `/bin/uptime` and the NTP client print
// them, and a clock nothing has corrected reports 0 and 0.
int64_t ktime_last_step(void);
uint32_t ktime_step_count(void);

#endif
