#ifndef ULIB_UDATE_H
#define ULIB_UDATE_H

// A `struct rtc_time` as "2026-09-04 14:05" or "2026-09-04 14:05:33" --
// the ISO shape ls, Properties and the File Manager's dialogs all print.
//
// Five programs formatted the same six fields in four different
// spellings. Two of those are deliberate and are NOT here: the file
// view's "09-04 14:05" is sized for a column, and /bin/stat keeps the
// kernel shell's "MM/DD/YYYY" so the two commands agree while both
// exist. Everything that wants a full date takes this one.
//
// A zeroed time (the kernel's per-entry stat failed) formats as an empty
// string rather than "0000-00-00 00:00", which reads as a date.
//
// **IT LOCALISES, because it is the DISPLAY path.** Every timestamp the
// kernel hands out is UTC -- the clock, and every filesystem entry --
// and a date shown to a person is local. Doing it here rather than at
// each call site is the same argument as the formatting itself: four
// spellings became one, and a fifth caller forgetting the conversion
// would show a date that is right by a number of hours nobody can see.
// Compare a STORED timestamp before formatting it, never after.
#include <stdio.h>
#include <time.h>
#include "rtctime.h"

static inline void rtc_format_iso(char *out, unsigned long cap,
                                  const struct rtc_time *t, int with_seconds) {
    if (!t->year && !t->month && !t->day) { if (cap) out[0] = '\0'; return; }
    struct rtc_time l = *t;
    tz_localize(&l);
    if (with_seconds)
        snprintf(out, cap, "%04u-%02u-%02u %02u:%02u:%02u", (unsigned)l.year,
                 (unsigned)l.month, (unsigned)l.day, (unsigned)l.hour,
                 (unsigned)l.minute, (unsigned)l.second);
    else
        snprintf(out, cap, "%04u-%02u-%02u %02u:%02u", (unsigned)l.year,
                 (unsigned)l.month, (unsigned)l.day, (unsigned)l.hour,
                 (unsigned)l.minute);
}

#endif // ULIB_UDATE_H
