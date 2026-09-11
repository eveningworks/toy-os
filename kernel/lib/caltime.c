// See caltime.h. Howard Hinnant's civil-from-days / days-from-civil,
// which this file is a transcription of rather than an invention.
//
// FREESTANDING, AND THAT IS LOAD-BEARING: this is compiled twice, once
// into the kernel and once into libc.a for ring 3's <time.h>. One
// kernel include here and the C library would need its own copy of the
// calendar -- which is exactly the split that made this a separate file
// in the first place.
#include "caltime.h"
#include "rtctime.h"

int cal_is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int cal_days_in_month(int year, int month) {
    static const int DAYS[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month < 1 || month > 12) return 0;
    if (month == 2 && cal_is_leap(year)) return 29;
    return DAYS[month - 1];
}

int cal_day_of_week(int year, int month, int day) {
    static const int T[] = { 0,3,2,5,0,3,5,1,4,6,2,4 };
    if (month < 1 || month > 12) return 0;
    if (month < 3) year -= 1;
    int d = (year + year/4 - year/100 + year/400 + T[month - 1] + day) % 7;
    // C's % keeps the sign of the dividend, so a year before year 0
    // would come back negative. Cheap to fix, and the alternative is a
    // function that is right for every input anyone has tried.
    return d < 0 ? d + 7 : d;
}

int64_t cal_days_from_civil(int year, int month, int day) {
    int y = year;
    int m = month;
    int d = day;
    y -= m <= 2;
    // Integer division truncates toward zero and this needs FLOOR
    // division: without the adjustment every date before year 0 lands
    // in the wrong era. Not reachable from this OS's hardware, and the
    // arithmetic is either right or it is not.
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;                                    // [0, 399]
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;   // [0, 365]
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            // [0, 146096]
    return (int64_t)era * 146097 + doe - 719468;                // since 1970-01-01
}

void cal_civil_from_days(int64_t days, int *year, int *month, int *day) {
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;                                     // [0, 146096]
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);              // [0, 365]
    int64_t mp = (5 * doy + 2) / 153;                                   // [0, 11]
    int64_t d = doy - (153 * mp + 2) / 5 + 1;                           // [1, 31]
    int64_t m = mp + (mp < 10 ? 3 : -9);                                // [1, 12]
    y += m <= 2;
    if (year) *year = (int)y;
    if (month) *month = (int)m;
    if (day) *day = (int)d;
}

int cal_day_of_year(int year, int month, int day) {
    return (int)(cal_days_from_civil(year, month, day) -
                 cal_days_from_civil(year, 1, 1));
}

uint64_t cal_rtc_to_epoch(const struct rtc_time *t) {
    int64_t days = cal_days_from_civil((int)t->year, (int)t->month, (int)t->day);
    // Years below 1970 cannot come off this hardware path (the RTC
    // reports a real current date); clamp rather than underflow the
    // unsigned result. cal_days_from_civil() returns a SIGNED day count
    // precisely so this decision is the caller's.
    if (days < 0) return 0;
    return (uint64_t)days * 86400u
         + (uint64_t)t->hour * 3600u
         + (uint64_t)t->minute * 60u
         + (uint64_t)t->second;
}

void cal_epoch_to_rtc(uint64_t epoch, struct rtc_time *out) {
    uint64_t days = epoch / 86400u;
    uint32_t rem = (uint32_t)(epoch % 86400u);
    out->hour = (uint8_t)(rem / 3600u);
    out->minute = (uint8_t)((rem % 3600u) / 60u);
    out->second = (uint8_t)(rem % 60u);

    int y, m, d;
    cal_civil_from_days((int64_t)days, &y, &m, &d);
    out->year = (uint16_t)y;
    out->month = (uint8_t)m;
    out->day = (uint8_t)d;
}
