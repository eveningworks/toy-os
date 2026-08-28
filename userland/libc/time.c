// C's <time.h> over the RTC and api/caltime.h. See the header for the
// one thing that matters -- gmtime() and localtime() are the same
// function here, because the system has no stored UTC offset and
// pretending otherwise would put a silent skew between time() and a
// file's timestamp.
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <caltime.h>
#include "rt/sys.h"
#include "proc_info.h"
#include "syscall_abi.h"

#define SECS_PER_DAY 86400

time_t time(time_t *t) {
    struct rtc_time now;
    // A failed read, or an RTC that answered year 0, reports the epoch
    // 0: a caller comparing against a file timestamp gets an
    // obviously-wrong small number rather than a plausible ancient date.
    if (sys_gettime(&now) != 0 || now.year == 0) {
        if (t) *t = 0;
        return 0;
    }
    int64_t days = cal_days_from_civil((int)now.year, (int)now.month, (int)now.day);
    time_t v = (time_t)days * SECS_PER_DAY +
               now.hour * 3600 + now.minute * 60 + now.second;
    if (t) *t = v;
    return v;
}

struct tm *gmtime_r(const time_t *t, struct tm *out) {
    if (!t || !out) return 0;
    time_t v = *t;
    // FLOOR division, not truncation: for a negative epoch C's / rounds
    // toward zero and would put times before 1970 on the wrong day.
    // Nothing in this OS produces one -- and an arithmetic function
    // that is right only for the inputs someone tried is how calendar
    // code goes wrong.
    int64_t days = v / SECS_PER_DAY;
    int64_t rem = v % SECS_PER_DAY;
    if (rem < 0) { rem += SECS_PER_DAY; days--; }

    int y, m, d;
    cal_civil_from_days(days, &y, &m, &d);
    out->tm_sec = (int)(rem % 60);
    out->tm_min = (int)((rem / 60) % 60);
    out->tm_hour = (int)(rem / 3600);
    out->tm_mday = d;
    out->tm_mon = m - 1;      // C counts months from 0
    out->tm_year = y - 1900;  // and years from 1900
    out->tm_wday = cal_day_of_week(y, m, d);
    out->tm_yday = cal_day_of_year(y, m, d);
    out->tm_isdst = 0;
    return out;
}

struct tm *localtime_r(const time_t *t, struct tm *out) { return gmtime_r(t, out); }

// The static-buffer forms C specifies. One buffer between them, because
// they are one function here and two buffers would imply otherwise.
static struct tm g_tm;
struct tm *gmtime(const time_t *t) { return gmtime_r(t, &g_tm); }
struct tm *localtime(const time_t *t) { return gmtime_r(t, &g_tm); }

time_t mktime(struct tm *tm) {
    if (!tm) return -1;
    // NORMALISE. Everything is folded into a seconds count and a day
    // count and then converted back, so an out-of-range field carries
    // instead of being rejected -- "tm_mday += 40; mktime(&tm)" is how
    // C does date arithmetic and it has to work.
    //
    // Months are normalised FIRST and separately, because a month is
    // not a fixed number of days: "March 32nd" only means April 1st
    // once the year and month are settled.
    int64_t mon = tm->tm_mon;
    int64_t year = (int64_t)tm->tm_year + 1900;
    year += mon / 12;
    mon %= 12;
    if (mon < 0) { mon += 12; year--; }

    int64_t days = cal_days_from_civil((int)year, (int)mon + 1, 1) + (tm->tm_mday - 1);
    int64_t secs = (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec;
    // The seconds may themselves be out of range in either direction.
    int64_t carry = secs / SECS_PER_DAY;
    secs %= SECS_PER_DAY;
    if (secs < 0) { secs += SECS_PER_DAY; carry--; }
    days += carry;

    time_t v = days * SECS_PER_DAY + secs;
    // C requires the struct to come back normalised, which is also what
    // makes mktime the way to fill in tm_wday and tm_yday.
    gmtime_r(&v, tm);
    return v;
}

// --- strftime --------------------------------------------------------

static const char *const WDAY[] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                    "Thursday", "Friday", "Saturday" };
static const char *const MON[] = { "January", "February", "March", "April",
                                   "May", "June", "July", "August",
                                   "September", "October", "November", "December" };

struct sb { char *p; size_t cap; size_t n; int over; };

static void sb_ch(struct sb *b, char c) {
    if (b->n + 1 >= b->cap) { b->over = 1; return; }
    b->p[b->n++] = c;
}
static void sb_str(struct sb *b, const char *s) { while (*s) sb_ch(b, *s++); }
static void sb_str_n(struct sb *b, const char *s, int n) {
    for (int i = 0; i < n && s[i]; i++) sb_ch(b, s[i]);
}
static void sb_num(struct sb *b, long v, int width, char pad) {
    char t[24];
    snprintf(t, sizeof t, "%ld", v);
    for (int i = (int)strlen(t); i < width; i++) sb_ch(b, pad);
    sb_str(b, t);
}

// Named so the compound conversions (%F, %T) can reuse the simple ones
// without a second switch to keep in step.
static void one(struct sb *b, char c, const struct tm *tm) {
    switch (c) {
    case 'Y': sb_num(b, tm->tm_year + 1900, 0, '0'); break;
    case 'y': sb_num(b, (tm->tm_year + 1900) % 100, 2, '0'); break;
    case 'm': sb_num(b, tm->tm_mon + 1, 2, '0'); break;
    case 'd': sb_num(b, tm->tm_mday, 2, '0'); break;
    case 'e': sb_num(b, tm->tm_mday, 2, ' '); break;
    case 'H': sb_num(b, tm->tm_hour, 2, '0'); break;
    case 'M': sb_num(b, tm->tm_min, 2, '0'); break;
    case 'S': sb_num(b, tm->tm_sec, 2, '0'); break;
    case 'j': sb_num(b, tm->tm_yday + 1, 3, '0'); break;
    case 'I': {
        // 12-hour: midnight and noon are both 12, which is the case a
        // plain `hour % 12` gets wrong in both directions.
        int h = tm->tm_hour % 12;
        sb_num(b, h == 0 ? 12 : h, 2, '0');
        break;
    }
    case 'p': sb_str(b, tm->tm_hour < 12 ? "AM" : "PM"); break;
    case 'a': if (tm->tm_wday >= 0 && tm->tm_wday < 7) sb_str_n(b, WDAY[tm->tm_wday], 3); break;
    case 'A': if (tm->tm_wday >= 0 && tm->tm_wday < 7) sb_str(b, WDAY[tm->tm_wday]); break;
    case 'b': case 'h': if (tm->tm_mon >= 0 && tm->tm_mon < 12) sb_str_n(b, MON[tm->tm_mon], 3); break;
    case 'B': if (tm->tm_mon >= 0 && tm->tm_mon < 12) sb_str(b, MON[tm->tm_mon]); break;
    // There is no zone name to report -- the offset is already baked
    // into the value (see <time.h>). "LOCAL" says that without claiming
    // a zone this system cannot name.
    case 'Z': sb_str(b, "LOCAL"); break;
    case '%': sb_ch(b, '%'); break;
    default:
        // Copied through literally, the same choice kfmt makes: a typo
        // shows up in the output instead of vanishing.
        sb_ch(b, '%');
        sb_ch(b, c);
        break;
    }
}

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    if (!s || !fmt || !tm || max == 0) return 0;
    struct sb b = { s, max, 0, 0 };
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { sb_ch(&b, *p); continue; }
        p++;
        if (!*p) { sb_ch(&b, '%'); break; }
        switch (*p) {
        // The compounds, expanded through one() so there is a single
        // definition of what %Y means.
        case 'F': one(&b, 'Y', tm); sb_ch(&b, '-'); one(&b, 'm', tm); sb_ch(&b, '-'); one(&b, 'd', tm); break;
        case 'T': one(&b, 'H', tm); sb_ch(&b, ':'); one(&b, 'M', tm); sb_ch(&b, ':'); one(&b, 'S', tm); break;
        case 'R': one(&b, 'H', tm); sb_ch(&b, ':'); one(&b, 'M', tm); break;
        case 'D': one(&b, 'm', tm); sb_ch(&b, '/'); one(&b, 'd', tm); sb_ch(&b, '/'); one(&b, 'y', tm); break;
        default:  one(&b, *p, tm); break;
        }
    }
    if (b.over) {
        // C: the contents are unspecified on overflow and the return is
        // 0. NUL-terminating anyway so a caller that ignores the return
        // gets a short string rather than an unterminated buffer.
        s[0] = '\0';
        return 0;
    }
    s[b.n] = '\0';
    return b.n;
}

// --- asctime / ctime --------------------------------------------------

static char g_asc[32];

static const char *const WDAY3[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
static const char *const MON3[]  = { "Jan","Feb","Mar","Apr","May","Jun",
                                     "Jul","Aug","Sep","Oct","Nov","Dec" };

char *asctime(const struct tm *tm) {
    if (!tm) return 0;
    // Built through strftime() rather than one snprintf, for one
    // concrete reason: C's asctime space-pads the day of month ("Jan
    // 5"), and kfmt's numeric width pads with ZEROES -- `%2d` of 5 is
    // "05" there, not " 5". strftime's %e is the space-padded form and
    // is this file's own code, so there is one place that knows the
    // difference. The trailing newline is part of the specification.
    const char *w = (tm->tm_wday >= 0 && tm->tm_wday < 7) ? WDAY3[tm->tm_wday] : "???";
    const char *m = (tm->tm_mon >= 0 && tm->tm_mon < 12) ? MON3[tm->tm_mon] : "???";
    char rest[24];
    strftime(rest, sizeof rest, "%e %H:%M:%S %Y", tm);
    snprintf(g_asc, sizeof g_asc, "%s %s %s\n", w, m, rest);
    return g_asc;
}

char *ctime(const time_t *t) { return asctime(gmtime(t)); }

// --- clock -----------------------------------------------------------

clock_t clock(void) {
    // REAL PROCESSOR TIME, not wall time. The kernel has tracked
    // per-process cpu_ns all along; what was missing was any way for a
    // process to find its OWN row -- SYS_PROC_INFO is indexed by
    // process-table slot. SYS_GETPID closed that, which is the only
    // reason this function exists rather than being another documented
    // absence.
    int me = sys_getpid();
    if (me < 0) return (clock_t)-1;   // no scheduler slot: C's "unavailable"
    struct proc_info pi;
    for (int i = 0; i < SYS_PROC_MAX; i++) {
        if (sys_proc_info(i, &pi) == 0 && pi.pid == me)
            return (clock_t)(pi.cpu_ns / 1000u);   // CLOCKS_PER_SEC is 1e6
    }
    return (clock_t)-1;
}
