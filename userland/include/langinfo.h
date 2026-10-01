#ifndef ULIB_LANGINFO_H
#define ULIB_LANGINFO_H

// POSIX's <langinfo.h>: the current locale's formats and names, by item.
// What setlocale() selected -- <locale.h> has the model. The strings are
// static and change on the next setlocale(); copy one you keep.
//
// The NAMES are English in every locale: a region chooses the order and
// the punctuation of a date, not the language of its month names.

typedef int nl_item;

enum {
    CODESET = 0,     // "ISO-8859-1": this system is Latin-1 end to end
    D_T_FMT,         // strftime %c
    D_FMT,           // strftime %x
    T_FMT,           // strftime %X
    T_FMT_AMPM,      // strftime %r
    AM_STR, PM_STR,
    DAY_1, DAY_2, DAY_3, DAY_4, DAY_5, DAY_6, DAY_7,           // Sunday first
    ABDAY_1, ABDAY_2, ABDAY_3, ABDAY_4, ABDAY_5, ABDAY_6, ABDAY_7,
    MON_1, MON_2, MON_3, MON_4, MON_5, MON_6,
    MON_7, MON_8, MON_9, MON_10, MON_11, MON_12,
    ABMON_1, ABMON_2, ABMON_3, ABMON_4, ABMON_5, ABMON_6,
    ABMON_7, ABMON_8, ABMON_9, ABMON_10, ABMON_11, ABMON_12,
    RADIXCHAR,       // LC_NUMERIC's decimal mark
    THOUSEP,         // and its digit-group separator ("" for none)
    YESEXPR, NOEXPR,

    // glibc's: the FIRST BYTE of the string is the week's first day,
    // 1 = Sunday, 2 = Monday.
    _NL_TIME_FIRST_WEEKDAY,
    // toy-os's own, because POSIX has no short forms: a time of day
    // without seconds (a list column, the taskbar's compact clock) and
    // the date written out ("Thursday 1 October 2026").
    _TOY_T_FMT_HM,
    _TOY_D_FMT_LONG,
    // "1" when calendars show ISO week numbers, "0" when not -- a
    // regional habit (Finland's calendars print them; the US's do not).
    _TOY_WEEK_NUMBERS,

    _NL_ITEM_COUNT
};

// The string for `item`, or "" for an item this does not know -- never
// NULL, as POSIX specifies.
char *nl_langinfo(nl_item item);

#endif
