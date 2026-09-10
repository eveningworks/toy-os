#ifndef ULIB_UPROGRESS_H
#define ULIB_UPROGRESS_H

// A one-line transfer meter, redrawn in place with `\r`.
//
// THE METER MUST NOT SHARE A STREAM WITH THE PAYLOAD. `wget` writes the
// body to stdout when it has no `-O`, so a meter there would corrupt
// every redirected download -- which is why the caller passes the fd
// and the answer is fd 2. `uprogress_begin()` then disables itself
// unless that fd is a terminal, as GNU wget and curl both do, so a
// captured stderr does not fill with repaint junk.
//
// REDRAWN ON A TIMER, not per chunk: a byte-count trigger fires
// thousands of times a second on a fast transfer and a handful of times
// on a slow one, so the cadence would track the thing being measured.
//
// The rate is the AVERAGE over the whole transfer, not a window. It is
// steadier to read and needs no history; the cost is that it lags a
// transfer whose speed changes, which is worth knowing before believing
// the ETA of one that just stalled.
#include <stdint.h>

struct uprogress {
    int fd;                        // -1 once disabled: nothing is drawn
    unsigned long long total;      // 0 when the size is not known ahead
    unsigned long long done;
    unsigned long long start_ns;
    unsigned long long last_ns;
    int width;                     // columns of the last line drawn
};

// `total` 0 means "not known": the meter then shows bytes, rate and
// elapsed time with no bar and no percentage, rather than guessing.
void uprogress_begin(struct uprogress *p, int fd, unsigned long long total);

// Account for `n` more bytes, redrawing if the cadence is due.
void uprogress_add(struct uprogress *p, unsigned long long n);

// Final redraw and a newline, so whatever prints next starts clean.
// Safe on a meter that was never begun or that disabled itself.
void uprogress_end(struct uprogress *p);

#endif
