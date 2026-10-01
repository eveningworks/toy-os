#ifndef ULIB_UDATE_H
#define ULIB_UDATE_H

// A `struct rtc_time` written the way the LC_TIME locale writes it --
// "1.10.2026 14.02" in Finland, "10/1/2026 2:02 PM" in the US. ls,
// stat, Properties, the File Manager and the taskbar all take this one,
// so a region changes every date on screen at once.
//
// `what` is UDATE_DATE and/or UDATE_TIME, plus UDATE_SECONDS (the time
// with seconds) or UDATE_LONG (the date written out). In the "C" locale
// -- a program that never called setlocale(LC_ALL, "") -- it is the ISO
// shape every caller printed before locales existed, so such a program's
// output does not move.
//
// A zeroed time (the kernel's per-entry stat failed) formats as an empty
// string rather than a date that reads as real.
//
// **IT LOCALISES, because it is the DISPLAY path.** Every timestamp the
// kernel hands out is UTC; a date shown to a person is local. Compare a
// STORED timestamp before formatting it, never after.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <locale.h>
#include <langinfo.h>
#include <caltime.h>
#include "rtctime.h"

enum { UDATE_DATE = 1, UDATE_TIME = 2, UDATE_SECONDS = 4, UDATE_LONG = 8 };

static inline void udate_format_tm(char *out, unsigned long cap, const struct tm *tm, int what) {
    int c = strcmp(setlocale(LC_TIME, 0), "C") == 0;
    const char *d = (what & UDATE_LONG) ? nl_langinfo(_TOY_D_FMT_LONG)
                  : c ? "%Y-%m-%d" : nl_langinfo(D_FMT);
    const char *t = (what & UDATE_SECONDS) ? (c ? "%H:%M:%S" : nl_langinfo(T_FMT))
                  : c ? "%H:%M" : nl_langinfo(_TOY_T_FMT_HM);
    char fmt[64];
    snprintf(fmt, sizeof fmt, "%s%s%s", (what & UDATE_DATE) ? d : "",
             (what & UDATE_DATE) && (what & UDATE_TIME) ? " " : "",
             (what & UDATE_TIME) ? t : "");
    if (!strftime(out, cap, fmt, tm) && cap) out[0] = '\0';
}

static inline void udate_format(char *out, unsigned long cap, const struct rtc_time *utc, int what) {
    if (!utc->year && !utc->month && !utc->day) { if (cap) out[0] = '\0'; return; }
    time_t e = (time_t)cal_rtc_to_epoch(utc);
    struct tm tm;
    localtime_r(&e, &tm);
    udate_format_tm(out, cap, &tm, what);
}

#endif // ULIB_UDATE_H
