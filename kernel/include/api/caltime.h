#ifndef CALTIME_H
#define CALTIME_H

#include <stdint.h>

// PURE CALENDAR ARITHMETIC -- the proleptic Gregorian calendar and
// nothing else. No timezone, no DST, no clock, no filesystem.
//
// WHY IT IS ITS OWN FILE. It used to live inside kernel/lib/tz.c, which
// also owns the /etc/timezones database, the DST rules and the
// persisted city choice -- so it reaches for fs.h, klog.h and
// etc_config.h and cannot be compiled into ring 3. The C library's
// <time.h> needs exactly this arithmetic and none of that machinery,
// and the alternative to splitting was a second implementation of
// Howard Hinnant's days_from_civil in userland, which is the
// duplication this toolkit exists to prevent. So: the arithmetic is
// here and shared (compiled into the kernel AND into libc.a), and tz.c
// keeps the policy.
//
// **THE EPOCH HERE IS UNITLESS ABOUT TIMEZONES.** These convert a
// calendar date to a day count and back; whether the date you hand in
// is UTC or local is the CALLER's business, and the answer is in the
// same reckoning. toy-os feeds these UTC throughout: the kernel's
// clock is UTC, every filesystem timestamp is a UTC epoch, and the
// conversion to a local time happens in ring 3 (userland/lib/utz.h).
//
// The algorithm is Hinnant's, valid for any year the int range holds
// and correct across the 100/400 leap rules -- not a table of month
// lengths with special cases, which is where hand-rolled calendar code
// goes wrong.

// The same conversion over a `struct rtc_time` (api/timer.h), which is
// the shape the clock and the filesystem both speak. Forward-declared
// rather than included, so this header stays free of the timer API.
//
// UNITLESS ABOUT TIMEZONES, exactly as the day-count pair below: what
// goes in decides what comes out. The kernel feeds these UTC now.
struct rtc_time;
uint64_t cal_rtc_to_epoch(const struct rtc_time *t);
void cal_epoch_to_rtc(uint64_t epoch, struct rtc_time *out);

int cal_is_leap(int year);
// 1..12 -> 28..31. Returns 0 for a month outside 1..12 rather than
// indexing its table with it.
int cal_days_in_month(int year, int month);
// 0 = Sunday .. 6 = Saturday (Sakamoto's algorithm).
int cal_day_of_week(int year, int month, int day);

// Days since 1970-01-01 for a civil date. NEGATIVE before 1970, which
// is the whole reason it returns a signed type -- a caller that cannot
// represent that must clamp, and tz.c does.
int64_t cal_days_from_civil(int year, int month, int day);
// The inverse. `days` may be negative.
void cal_civil_from_days(int64_t days, int *year, int *month, int *day);

// Day of the year, 0-based (0 = January 1) -- what a struct tm's
// tm_yday wants.
int cal_day_of_year(int year, int month, int day);

#endif
