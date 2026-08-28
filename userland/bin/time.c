// time -- the date and time, in the configured timezone.
//
// NOT coreutils' `time` (which measures how long a command takes).
// This is `date`, under the name this shell has always used for it.
// The measuring one would be a different program and is not built; see
// SYS_MONOTONIC_NS and /bin/uptime for the interval side.
//
// SYS_GETTIME ALREADY RETURNS LOCAL TIME -- rtc_read_local() applies
// the configured offset kernel-side, so there is no conversion here and
// no second copy of the timezone table. The city NAME comes from the
// settings registry (`system.timezone`), which is the same string
// `config get` prints, so the two cannot disagree about where this
// machine thinks it is.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>

// Spelled out rather than derived: three-letter abbreviations would be
// shorter and are ambiguous across languages, and the kernel shell's
// own version used the same full-ish names.
static const char *const MONTHS[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

// The configured timezone's city name, or "" if the setting cannot be
// read. Empty is a real answer -- a kernel with no timezone registered
// still has a clock -- so the caller prints the time either way rather
// than failing over a label.
static void timezone_name(char *out, unsigned long cap) {
    struct setting_msg msg;
    out[0] = '\0';
    memset(&msg, 0, sizeof msg);
    msg.op = SETTING_OP_GET;
    strlcpy(msg.name, "system.timezone", sizeof msg.name);
    if (sys_setting(&msg) == 0) strlcpy(out, msg.value, cap);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct rtc_time t;
    if (sys_gettime(&t) != 0) {
        cmd_fail("time", 0);
        return 1;
    }

    char tz[SETTING_ABI_VALUE_MAX];
    timezone_name(tz, sizeof tz);

    // The month index is bounds-checked rather than trusted: it comes
    // from CMOS, and a battery-dead RTC reports values no calendar has.
    const char *mon = (t.month >= 1 && t.month <= 12) ? MONTHS[t.month - 1] : "???";

    char line[128];
    snprintf(line, sizeof line, "%s %02u, %u  %02u:%02u:%02u%s%s\n",
             mon, t.day, t.year, t.hour, t.minute, t.second,
             tz[0] ? "  " : "", tz);
    sys_print(line);
    return 0;
}
