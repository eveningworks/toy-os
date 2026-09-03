// The wall clock: an epoch anchored to the monotonic clocksource.
//
// See api/ktime.h for the contract. The mechanism is two numbers -- the
// epoch at some instant, and the clocksource reading at that same
// instant -- and every query is the difference between now and the
// anchor added to the epoch. Setting the clock re-anchors both.
#include "ktime.h"
#include "caltime.h"
#include "clocksource.h"
#include "klog.h"
#include "timer.h"

#define NS_PER_SEC 1000000000ull

// THE ANCHOR: `g_epoch_ns` is what the wall clock read when
// clocksource_now_ns() read `g_mono_ns`. Both are written together and
// must never be updated apart -- a torn pair is a clock that jumps by
// however long the two writes were separated by.
static uint64_t g_epoch_ns;
static uint64_t g_mono_ns;
static int g_ready;

static int64_t g_last_step;
static uint32_t g_step_count;

// The RTC's reading as an epoch. Zero when the hardware answers with a
// date no calendar has, which a machine with a dead CMOS battery does:
// year 2000 with a month of 0 is not a time to anchor to.
static uint64_t rtc_epoch(void) {
    struct rtc_time t;
    rtc_read(&t);
    if (t.year < 1970 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 ||
        t.hour > 23 || t.minute > 59 || t.second > 60) {
        return 0;
    }
    int64_t days = cal_days_from_civil(t.year, t.month, t.day);
    if (days < 0) return 0;
    return (uint64_t)days * 86400ull + t.hour * 3600ull + t.minute * 60ull + t.second;
}

void ktime_init(void) {
    // ORDER MATTERS: read the clocksource FIRST, then the RTC. rtc_read()
    // spins on the CMOS update-in-progress flag for up to a second, and
    // anchoring to a monotonic reading taken after that spin would date
    // the epoch to before the time it was measured at.
    g_mono_ns = clocksource_now_ns();
    uint64_t sec = rtc_epoch();
    g_epoch_ns = sec * NS_PER_SEC;
    g_ready = 1;

    if (!sec) {
        klog_write("ktime: the hardware clock reads a date no calendar has -- "
                   "wall time starts at the epoch until something sets it\n");
    }
}

uint64_t ktime_now_ns(void) {
    if (!g_ready) return 0;
    uint64_t mono = clocksource_now_ns();
    // The clocksource is monotonic, so this subtraction cannot wrap --
    // but it is guarded anyway, because "cannot happen" plus unsigned
    // arithmetic is how a clock reports the year 586 billion.
    uint64_t delta = mono > g_mono_ns ? mono - g_mono_ns : 0;
    return g_epoch_ns + delta;
}

uint64_t ktime_now_sec(void) {
    return ktime_now_ns() / NS_PER_SEC;
}

// An epoch second as broken-down civil time. Split out of ktime_read()
// so ktime_set() can convert the value it is WRITING rather than
// re-reading the clock it has just moved.
static void epoch_to_rtc(uint64_t sec, struct rtc_time *out) {
    uint64_t days = sec / 86400ull;
    uint32_t rem = (uint32_t)(sec % 86400ull);

    int y, m, d;
    cal_civil_from_days((int64_t)days, &y, &m, &d);
    out->year = (uint16_t)y;
    out->month = (uint8_t)m;
    out->day = (uint8_t)d;
    out->hour = (uint8_t)(rem / 3600);
    out->minute = (uint8_t)((rem % 3600) / 60);
    out->second = (uint8_t)(rem % 60);
}

void ktime_read(struct rtc_time *out) {
    if (!out) return;
    epoch_to_rtc(ktime_now_sec(), out);
}

int ktime_set(uint64_t sec, uint32_t nsec) {
    // 1970-01-01 .. 9999-12-31, the range the RTC's two-digit year plus
    // an assumed century and cal_civil_from_days() can both hold.
    if (sec > 253402300799ull) return 0;
    if (nsec >= NS_PER_SEC) return 0;

    int64_t before = (int64_t)ktime_now_sec();

    g_mono_ns = clocksource_now_ns();
    g_epoch_ns = sec * NS_PER_SEC + nsec;
    g_ready = 1;

    g_last_step = (int64_t)sec - before;
    g_step_count++;

    // THE RTC HAS NO SUB-SECOND FIELD, so this ROUNDS to the nearest
    // second rather than truncating: half a second of error at the next
    // boot instead of up to a whole one. Rounded BEFORE the calendar
    // conversion, so a value half a second before midnight rolls the
    // date over correctly instead of being clamped.
    struct rtc_time t;
    epoch_to_rtc(sec + (nsec >= NS_PER_SEC / 2 ? 1 : 0), &t);
    // A FAILED RTC WRITE IS NOT A FAILED SET. The correction is already
    // live; all that is lost is its surviving a reboot, and saying so is
    // more useful than refusing a call that mostly worked.
    if (!rtc_write(&t)) {
        klog_write("ktime: the clock was set but the RTC would not take it -- "
                   "the correction is lost at the next boot\n");
    }
    return 1;
}

int64_t ktime_last_step(void) { return g_last_step; }
uint32_t ktime_step_count(void) { return g_step_count; }
