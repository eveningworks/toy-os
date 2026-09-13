#ifndef ULIB_TIME_H
#define ULIB_TIME_H

// C's <time.h>, over the RTC and the shared calendar arithmetic
// (api/caltime.h, compiled into libc.a).
//
// **time() IS UTC, AND localtime() IS A REAL CONVERSION.**
//
// The kernel's clock is UTC, SYS_GETTIME returns UTC and every
// filesystem timestamp is a UTC epoch, so an epoch from time() and one
// from a file's st.modified are the same kind of number and comparing
// them means something. localtime() applies the selected city's offset
// and its DST rule, both read from /etc/timezones by libc/tz.c -- which
// is where a C library keeps them, the kernel having stopped carrying a
// city database (api/tz.h).
//
// This header used to say the opposite, in detail: gmtime() and
// localtime() WERE the same function, and time() was local-derived,
// because the kernel converted at the syscall boundary and there was no
// stored UTC offset for libc to apply. It predicted that the fix would
// be a system-wide one rather than a libc patch. It was.
//
// **tzset() IS WHAT RE-READS THE SELECTION.** The city is read once, on
// the first conversion; a program that wants to follow a change made in
// System Settings calls tzset() again. Nothing polls a file from inside
// localtime().
//
// **clock() REPORTS REAL PROCESSOR TIME**, not wall time. The kernel has
// tracked per-process `cpu_ns` all along (abi/proc_info.h); what was
// missing was any way for a process to find its own row, since
// SYS_PROC_INFO is indexed by process-table slot. SYS_GETPID closed
// that. It returns (clock_t)-1 for a caller with no scheduler slot,
// which is C's "unavailable".
#include <stddef.h>
#include <stdint.h>

#ifndef __ULIB_TIME_T
#define __ULIB_TIME_T
typedef int64_t time_t;
#endif

struct tm {
    int tm_sec;    // 0..60 (60 for a leap second that will never arrive here)
    int tm_min;    // 0..59
    int tm_hour;   // 0..23
    int tm_mday;   // 1..31
    int tm_mon;    // 0..11  -- NOT 1..12, C's oldest trap
    int tm_year;   // years since 1900 -- likewise
    int tm_wday;   // 0..6, Sunday first
    int tm_yday;   // 0..365
    int tm_isdst;  // always 0: the offset is already baked in, so there
                   // is nothing here for a caller to correct for
};

// Seconds since 1970-01-01 in the system's own reckoning (see above).
// Also stores it through `t` when that is not NULL, as C specifies.
time_t time(time_t *t);

typedef int64_t clock_t;
// MICROSECONDS. C only requires that clock()/CLOCKS_PER_SEC be seconds;
// a microsecond tick keeps the arithmetic exact against the kernel's
// nanosecond counter and is what POSIX fixes it at.
#define CLOCKS_PER_SEC 1000000
clock_t clock(void);

// Both convert in the same reckoning -- see the header comment. The
// result is in a STATIC buffer that the next call overwrites, which is
// what C specifies and is why a caller keeping two of them must copy.
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
// The reentrant forms, which is what to reach for in new code.
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime_r(const time_t *t, struct tm *out);

// Re-reads the selected city and the database. Called for you by the
// first localtime(); call it again to pick up a change.
void tzset(void);

// POSIX's three globals, set by tzset(). `timezone` is SECONDS WEST of
// UTC (POSIX's sign, the opposite of the database's minutes east),
// `daylight` says whether the zone has a DST rule at all -- not whether
// it is in effect now, which is `tm_isdst` on a converted time.
extern char *tzname[2];
extern long timezone;
extern int daylight;

// The selected city's NAME -- the token, as stored. Not POSIX; here
// because `tzname` carries the display name and a caller that wants to
// report what is configured needs the identity.
const char *tz_current_name(void);

// The same conversion for the broken-down UTC time SYS_GETTIME returns,
// in place -- so reading the clock and showing it is two calls and no
// epoch round trip. `struct rtc_time` is abi/rtctime.h.
struct rtc_time;
void tz_localize(struct rtc_time *t);

// Seconds EAST of UTC at that moment, DST included. `timezone` above is
// the STANDARD-time offset and counts west, so the two differ in both
// sign and season; this is the one to use for arithmetic.
long tz_offset_seconds(const time_t *t);

// tm -> time_t, NORMALISING the input: a tm_mday of 32 or a tm_mon of
// 12 is carried into the next month or year rather than rejected,
// because that is how C says date arithmetic is done ("add 40 days,
// then mktime"). The struct is updated in place to the normalised
// values, tm_wday and tm_yday included.
time_t mktime(struct tm *tm);

// The UTC counterpart of mktime(): reads `tm` as UTC. POSIX, and the
// half of mktime that does the calendar arithmetic.
time_t timegm(struct tm *tm);

static inline double difftime(time_t a, time_t b) { return (double)(a - b); }

// The subset of conversions this implements: %Y %m %d %H %M %S %y %j
// %e %a %A %b %B %p %I %Z %% and %F %T %D %R (the common compounds).
// AN UNKNOWN CONVERSION IS COPIED THROUGH LITERALLY, the same choice
// kfmt makes, so a typo is visible rather than silently dropped.
// Returns the length written, or 0 if it would not fit -- in which case
// the buffer contents are unspecified, as C says.
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);

// The fixed 26-byte "Www Mmm dd hh:mm:ss yyyy\n" forms, static buffer.
char *asctime(const struct tm *tm);
char *ctime(const time_t *t);

#endif
