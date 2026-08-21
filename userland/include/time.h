#ifndef ULIB_TIME_H
#define ULIB_TIME_H

// C's <time.h>, over the RTC and the shared calendar arithmetic
// (api/caltime.h, compiled into libc.a).
//
// **THE ONE THING TO READ BEFORE USING THIS: gmtime() AND localtime()
// ARE THE SAME FUNCTION HERE, and time() is not UTC.**
//
// toy-os has no stored UTC offset. The RTC is read as LOCAL civil time
// with the selected city's offset and DST already applied (tz.h), and
// the filesystem stores epochs derived from that same local reckoning
// -- deliberately, and documented at those call sites: it makes
// timestamps arithmetic-comparable without inventing UTC handling the
// system does not have.
//
// So this header keeps the system honest rather than papering over it.
// time() returns an epoch in the SAME reckoning as a file's
// st.modified, so comparing them is meaningful -- which is the thing
// programs actually do. Making time() return true UTC while the
// filesystem's epochs stayed local would have put a silent offset
// between two numbers that look comparable, which is worse than a
// documented simplification.
//
// The fix is a system-wide one (a stored UTC offset, on the roadmap),
// not a libc patch -- and when it lands, gmtime() and localtime() here
// become genuinely different and nothing else in this header changes.
//
// **clock() REPORTS REAL PROCESSOR TIME**, not wall time. The kernel has
// tracked per-process `cpu_ns` all along (abi/proc_info.h); what was
// missing was any way for a process to find its own row, since
// SYS_PROC_INFO is indexed by process-table slot. SYS_GETPID closed
// that. It returns (clock_t)-1 for a caller with no scheduler slot,
// which is C's "unavailable".
#include <stddef.h>
#include <stdint.h>

typedef int64_t time_t;

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

// tm -> time_t, NORMALISING the input: a tm_mday of 32 or a tm_mon of
// 12 is carried into the next month or year rather than rejected,
// because that is how C says date arithmetic is done ("add 40 days,
// then mktime"). The struct is updated in place to the normalised
// values, tm_wday and tm_yday included.
time_t mktime(struct tm *tm);

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
