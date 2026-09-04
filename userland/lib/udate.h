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
#include <stdio.h>
#include "timer.h"

static inline void rtc_format_iso(char *out, unsigned long cap,
                                  const struct rtc_time *t, int with_seconds) {
    if (!t->year && !t->month && !t->day) { if (cap) out[0] = '\0'; return; }
    if (with_seconds)
        snprintf(out, cap, "%04u-%02u-%02u %02u:%02u:%02u", (unsigned)t->year,
                 (unsigned)t->month, (unsigned)t->day, (unsigned)t->hour,
                 (unsigned)t->minute, (unsigned)t->second);
    else
        snprintf(out, cap, "%04u-%02u-%02u %02u:%02u", (unsigned)t->year,
                 (unsigned)t->month, (unsigned)t->day, (unsigned)t->hour,
                 (unsigned)t->minute);
}

#endif // ULIB_UDATE_H
