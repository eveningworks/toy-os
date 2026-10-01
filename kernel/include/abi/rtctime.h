#ifndef RTCTIME_H
#define RTCTIME_H

#include <stdint.h>

// A BROKEN-DOWN WALL-CLOCK TIME, and nothing else.
//
// It lives in abi/ because it crosses the ring boundary: SYS_GETTIME
// writes one into a ring-3 buffer and `struct sys_dirent` embeds one
// (abi/syscall_abi.h). It was declared in api/timer.h beside the PIT
// and the CMOS, which meant anything wanting the TYPE also pulled in
// the kernel's timer prototypes -- and `kernel/lib/caltime.c` is
// compiled into libc.a, where a kernel include is exactly the mistake
// its own header comment warns about.
//
// **THE VALUES ARE UTC.** The kernel's clock is UTC, every filesystem
// timestamp is a UTC epoch, and converting to a local time is ring 3's
// job (userland/libc/tz.c). Nothing here records a zone, so a caller
// that has converted one of these knows that only because it did it.
struct rtc_time {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t day;
    uint8_t month;
    uint16_t year;
};

#endif
