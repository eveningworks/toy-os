// dmesg -- the kernel log, from ring 3.
//
// It was a ring-0 shell builtin until now, and not by choice: the log
// had no way out of the kernel. klog_dump() streams into a callback,
// which is the wrong shape for a syscall that must fill a buffer and
// return, so `dmesg` could not follow `ps`, `meminfo`, `kstack` and the
// rest into /bin -- and a Terminal window, which is where a person
// actually reads a log, could not run it at all. QUERY_KLOG is the way
// out; this is the reader.
//
// **PAGINATION IS GONE ON PURPOSE.** The builtin drew `-- more --` and
// blocked on a keystroke, which meant it could not be used in a GUI
// callback and had to detect that case and behave differently. This
// writes to stdout and stops. `dmesg | less` pages it, `dmesg -n 40`
// tails it, and both are somebody else's well-tested code -- which is
// what CLAUDE.md's rule against a builtin shadowing a /bin program that
// does more is about.
//
// THE RING MOVES WHILE YOU READ IT, and the offsets are what make that
// survivable. Every record says the absolute offset its first byte came
// from; if that is further along than where the previous record ended,
// the kernel logged enough during the walk to overwrite what was about
// to be read. That is reported rather than papered over -- Linux's
// dmesg prints a '-' for the same event on /dev/kmsg. A log that
// silently splices two eras together is worse than one with a gap in
// it, because only the second kind can be noticed.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "query_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put(const char *s, unsigned n) { write(1, s, n); }
static void puts_(const char *s) { put(s, (unsigned)strlen(s)); }

// How long --follow waits between polls. The log is not a stream that
// can be blocked on -- QUERY_KLOG is a snapshot interface, deliberately
// (a fact is computed on every read and has no stored form) -- so
// following is a poll, and the interval is the whole cost of it. 200 ms
// is under what a person notices and over what would show up in a CPU
// figure.
#define FOLLOW_POLL_MS 200

// Reads from `from` to the end of the log, writing to stdout. Returns
// the absolute offset just past the last byte written, or `from` when
// there was nothing new. `gaps` counts slices whose start had already
// aged out.
static unsigned long long drain(unsigned long long from, int *gaps) {
    struct query_klog r;
    unsigned long long next = from;

    for (int i = 0; ; i++) {
        if (sys_query_record(QUERY_KLOG, (unsigned)i, &r, sizeof r) <= 0) break;
        if (!r.len) continue;

        unsigned long long end = r.first + r.len;
        if (end <= next) continue;   // wholly behind us, already printed

        const char *p = (const char *)r.data;
        unsigned n = r.len;
        if (r.first < next) {
            // Overlaps what we already printed -- skip the prefix rather
            // than repeat it. Happens on --follow, where a slice
            // straddles the point the previous pass stopped at.
            unsigned skip = (unsigned)(next - r.first);
            p += skip;
            n -= skip;
        } else if (r.first > next && next != 0) {
            // A HOLE. The kernel overwrote bytes between the last thing
            // printed and this slice, so say so where it happened
            // rather than at the end -- a gap's position is most of its
            // meaning.
            char note[96];
            snprintf(note, sizeof note,
                     "\n[dmesg: %llu bytes lost -- the log wrapped while reading]\n",
                     (unsigned long long)(r.first - next));
            puts_(note);
            if (gaps) (*gaps)++;
        }
        put(p, n);
        next = end;
    }
    return next;
}

// The absolute offset of the start of the Nth-from-last line, so `-n`
// can tail without buffering the log twice. Walks the records counting
// newlines backwards is not possible on a forward-only interface, so
// this makes ONE pass recording where each line began, keeping only the
// last N -- a ring of offsets, not of text.
#define TAIL_MAX 256
static unsigned long long tail_start(int lines) {
    if (lines > TAIL_MAX) lines = TAIL_MAX;
    static unsigned long long starts[TAIL_MAX];
    int n = 0, count = 0;

    struct query_klog r;
    unsigned long long here = 0;
    int first_record = 1;
    for (int i = 0; ; i++) {
        if (sys_query_record(QUERY_KLOG, (unsigned)i, &r, sizeof r) <= 0) break;
        if (first_record) { here = r.first; first_record = 0; starts[n++] = here; count = 1; }
        for (unsigned j = 0; j < r.len; j++) {
            here = r.first + j + 1;
            if (r.data[j] != '\n') continue;
            // The line STARTS after the newline. Recorded into a ring so
            // a 16 KB log costs 2 KB of offsets rather than a second
            // copy of itself.
            starts[n % TAIL_MAX] = here;
            n++;
            count++;
        }
    }
    if (count <= lines) return 0;               // fewer lines than asked for
    // n has wrapped; the line we want is `lines` back from the newest.
    int idx = (n - lines) % TAIL_MAX;
    if (idx < 0) idx += TAIL_MAX;
    return starts[idx];
}

int main(int argc, char **argv) {
    int follow = 0, lines = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--follow") == 0) {
            follow = 1;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            lines = atoi(argv[++i]);
            if (lines <= 0) {
                puts_("dmesg: -n wants a positive line count\n");
                return 1;
            }
        } else {
            cmd_usage("dmesg [-n <lines>] [-w|--follow]");
            return 1;
        }
    }

    int gaps = 0;
    unsigned long long next = lines ? tail_start(lines) : 0;
    next = drain(next, &gaps);

    if (!follow) return 0;

    // --follow. There is no way out but a signal -- Ctrl-C, which is
    // the same answer `dmesg -w` and `tail -f` give, and which works
    // here because the shell puts this in its own process group. No
    // key handling of its own: this program never reads the keyboard,
    // so it can run in a Terminal window without taking a key from
    // anything else.
    for (;;) {
        sys_sleep_ms(FOLLOW_POLL_MS);
        next = drain(next, &gaps);
    }
}
