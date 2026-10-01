// time -- the date and time, in the configured timezone and region; and
// with -s, setting them.
//
// NOT coreutils' `time` (which measures how long a command takes).
// This is `date`, under the name this shell has always used for it.
// The measuring one would be a different program and is not built; see
// SYS_MONOTONIC_NS and /bin/uptime for the interval side.
//
// THE KERNEL'S CLOCK IS UTC. The city and its DST rule are libc's
// (tz.c), the spelling is the LC_TIME locale's (lib/udate.h), and the
// city NAME is the settings registry's `system.timezone` -- the string
// `config get` prints.
//
// -s TAKES ISO 8601 WHATEVER THE REGION, as `date -s` and timedatectl
// accept it: an input format that changed with a setting would make a
// script that sets the clock depend on that setting. It is LOCAL time,
// and it is REFUSED while network time is on, as `timedatectl set-time`
// refuses -- the next sync would undo it.
#include "rt/sys.h"
#include <time.h>
#include <locale.h>
#include "lib/cmd.h"
#include "lib/udate.h"
#include "lib/usetting.h"
#include <stdio.h>
#include <string.h>

#define USAGE "time [-s \"YYYY-MM-DD HH:MM[:SS]\"]"

// Exactly `n` digits at *p, advancing past them; -1 if anything else.
static int digits(const char **p, int n) {
    int v = 0;
    for (int i = 0; i < n; i++, (*p)++) {
        if (**p < '0' || **p > '9') return -1;
        v = v * 10 + (**p - '0');
    }
    return v;
}

// "YYYY-MM-DD HH:MM" or "...:SS", REJECTED rather than guessed: a
// field out of range is an error, not a carry into the next month.
static int parse_iso(const char *s, struct tm *tm) {
    memset(tm, 0, sizeof *tm);
    int y = digits(&s, 4);
    if (y < 0 || *s++ != '-') return 0;
    int mo = digits(&s, 2);
    if (mo < 1 || mo > 12 || *s++ != '-') return 0;
    int d = digits(&s, 2);
    if (d < 1 || (*s != ' ' && *s != 'T')) return 0;
    s++;
    int h = digits(&s, 2);
    if (h < 0 || h > 23 || *s++ != ':') return 0;
    int mi = digits(&s, 2);
    if (mi < 0 || mi > 59) return 0;
    int sec = 0;
    if (*s == ':') {
        s++;
        sec = digits(&s, 2);
        if (sec < 0 || sec > 59) return 0;
    }
    if (*s) return 0;
    // The day against ITS month, which mktime() would carry instead.
    struct tm probe = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d };
    timegm(&probe);
    if (probe.tm_mday != d) return 0;
    tm->tm_year = y - 1900; tm->tm_mon = mo - 1; tm->tm_mday = d;
    tm->tm_hour = h; tm->tm_min = mi; tm->tm_sec = sec;
    return 1;
}

static int set_clock(const char *arg) {
    char ntp[SETTING_ABI_VALUE_MAX] = "";
    usetting_get("system.ntp", ntp, sizeof ntp);
    if (strcmp(ntp, "on") == 0) {
        sys_print("time: network time is on -- turn off \"Set the time "
                  "automatically\" (system.ntp) first\n");
        return 1;
    }
    struct tm tm;
    if (!parse_iso(arg, &tm)) {
        sys_print("time: not a date and time: ");
        sys_print(arg);
        sys_print("\n");
        cmd_usage(USAGE);
        return 1;
    }
    time_t utc = mktime(&tm);
    if (utc <= 0 || sys_settime((uint64_t)utc, 0) != 0) {
        cmd_fail("time", arg);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    if (argc == 3 && strcmp(argv[1], "-s") == 0) {
        if (set_clock(argv[2])) return 1;
    } else if (argc != 1) {
        cmd_usage(USAGE);
        return 1;
    }

    struct rtc_time t;
    if (sys_gettime(&t) != 0) {
        cmd_fail("time", 0);
        return 1;
    }
    // A battery-dead RTC reads as year 0, which udate_format() prints as
    // nothing -- better than a date that looks real.
    char date[64], clock[32], tz[SETTING_ABI_VALUE_MAX] = "";
    udate_format(date, sizeof date, &t, UDATE_DATE | UDATE_LONG);
    udate_format(clock, sizeof clock, &t, UDATE_TIME | UDATE_SECONDS);
    usetting_get("system.timezone", tz, sizeof tz);

    char line[160];
    snprintf(line, sizeof line, "%s  %s%s%s\n", date, clock, tz[0] ? "  " : "", tz);
    sys_print(line);
    return 0;
}
