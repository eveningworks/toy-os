#ifndef ULIB_SYS_TIME_H
#define ULIB_SYS_TIME_H

// struct timeval and gettimeofday(), the pre-POSIX-2008 way to ask the
// time. <time.h> has clock_gettime() and is what new code should use;
// this exists because ported code includes it (dash from three files)
// and because struct timeval is the argument type of several older
// interfaces.

#include <sys/types.h>
#include <time.h>

struct timeval {
    time_t      tv_sec;   // seconds
    suseconds_t tv_usec;  // MICROseconds -- timespec's tv_nsec is nanoseconds
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

// **THE TIMEZONE ARGUMENT IS IGNORED, AND SO IT IS EVERYWHERE.** POSIX
// marked it obsolete and says the behaviour is unspecified when it is
// not NULL; Linux fills it with zeroes. Passing one here is accepted and
// zeroed rather than refused, because a caller that passes it is not
// reading it. A LOCAL time is ring 3's to compute from the UTC this
// returns -- see <time.h>'s tzset()/localtime().
//
// Returns 0, or -1 with errno set.
int gettimeofday(struct timeval *tv, void *tz);

#endif
