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
#include <time.h>

// --- absolute timestamps (-T) ------------------------------------------
//
// THE CONVERSION IS DERIVED, NOT RECORDED, and that is the whole design.
// The kernel stamps each line with MONOTONIC time as TEXT -- "[5068.88] "
// is characters in the ring, not a field (see kernel/lib/klog.c) -- so
// there is no wall clock stored anywhere to print. `-T` computes the
// boot instant once as `now - uptime` and adds each line's offset to it,
// which is exactly what Linux's `dmesg -T` does.
//
// Its inherited caveat, which util-linux also documents: the answer is
// WRONG IF THE CLOCK MOVED SINCE BOOT. A machine whose RTC was wrong
// until someone set it -- a dead CMOS battery, say -- reports every line
// shifted by however far the clock was out. Nothing here can detect
// that; only a wall clock recorded AT the line could, and the ring holds
// no room for one.
//
// A LINE FILTER RATHER THAN A REWRITE OF drain(), because the log
// arrives as byte slices whose boundaries fall wherever the ring's
// records do -- a line is routinely split across two. Everything that
// reaches stdout goes through here so the gap notes keep their place in
// the order.
#define ABS_LINE_MAX 512

static int g_abs;                  // -T
// --raw, and -l. The kernel writes its level into the ring's own
// timestamp -- "[7.03] <3> " -- so it is TEXT here rather than a field,
// and hiding it by default is what keeps every existing reader of this
// program's output working (util-linux's dmesg hides it the same way,
// and shows it for --raw).
static int g_raw;
static int g_min_level = 7;        // KLOG_LEVEL_DEBUG -- everything
static long long g_boot_epoch;     // seconds, or 0 when unknown
static char g_line[ABS_LINE_MAX];
static unsigned g_line_len;
static int g_line_over;            // this line outran the buffer

static void raw(const char *s, unsigned n) { write(1, s, n); }

// "<N> " at `at`, or -1 when the line carries none -- which a line the
// ring wrapped into the middle of legitimately does not.
static int line_level(const char *p, unsigned n, unsigned at) {
    if (at + 3 > n) return -1;
    if (p[at] != '<' || p[at + 2] != '>') return -1;
    if (p[at + 1] < '0' || p[at + 1] > '7') return -1;
    return p[at + 1] - '0';
}

// Writes one buffered line: drops it if its level is below the filter,
// hides the level marker unless --raw asked for it, and under -T
// replaces the leading "[secs.hh] " with the absolute time it names. A
// line without that prefix passes untouched -- the kernel stamps once
// per LOGICAL line, and inventing a time for a continuation would be a
// fabrication.
static void emit_line(const char *p, unsigned n) {
    unsigned secs = 0, i = 1, after_stamp = 0;
    if (!g_line_over && n > 2 && p[0] == '[') {
        while (i < n && p[i] >= '0' && p[i] <= '9') { secs = secs * 10 + (unsigned)(p[i] - '0'); i++; }
        // "[N.hh] " -- the hundredths are dropped on purpose: a whole
        // second is the resolution an absolute stamp can honestly claim
        // when its origin was derived from a one-second RTC.
        if (i > 1 && i + 4 <= n && p[i] == '.' && p[i + 3] == ']' && p[i + 4] == ' ')
            after_stamp = i + 5;
    }

    int level = after_stamp ? line_level(p, n, after_stamp) : -1;
    // A line with no level of its own is never filtered out: it is
    // evidence the ring wrapped into, and dropping it would hide that.
    if (level >= 0 && level > g_min_level) return;
    unsigned skip = (level >= 0 && !g_raw) ? 4 : 0;   // "<N> "

    if (!after_stamp) { raw(p, n); return; }

    if (g_abs) {
        time_t t = (time_t)(g_boot_epoch + (long long)secs);
        struct tm tm;
        char stamp[40];
        localtime_r(&t, &tm);
        unsigned len = (unsigned)strftime(stamp, sizeof stamp, "[%Y-%m-%d %H:%M:%S] ", &tm);
        if (len) raw(stamp, len);
        else     raw(p, after_stamp);
    } else {
        raw(p, after_stamp);
    }
    raw(p + after_stamp + skip, n - after_stamp - skip);
}

// EVERY byte goes through the line buffer now, not only under -T: the
// level sits in the text, so hiding it is a per-line edit whatever else
// was asked for.
static void put(const char *s, unsigned n) {
    for (unsigned k = 0; k < n; k++) {
        if (g_line_len < sizeof g_line) {
            g_line[g_line_len++] = s[k];
        } else {
            // Too long to hold. Emit what is buffered and pass the rest
            // of the line straight through rather than truncating it --
            // a half-line is a wrong line, and this file's whole point
            // is not silently splicing a log.
            if (!g_line_over) { emit_line(g_line, g_line_len); g_line_len = 0; g_line_over = 1; }
            raw(&s[k], 1);
        }
        if (s[k] == '\n') {
            if (!g_line_over) emit_line(g_line, g_line_len);
            g_line_len = 0;
            g_line_over = 0;
        }
    }
}

// A log that does not end in a newline still has a last line.
static void put_flush(void) {
    if (g_line_len && !g_line_over) emit_line(g_line, g_line_len);
    g_line_len = 0;
}

static void puts_(const char *s) { put(s, (unsigned)strlen(s)); }

// How long --follow waits between polls. The log is not a stream that
// can be blocked on -- QUERY_KLOG is a snapshot interface, deliberately
// (a fact is computed on every read and has no stored form) -- so
// following is a poll, and the interval is the whole cost of it. 200 ms
// is under what a person notices and over what would show up in a CPU
// figure.
#define FOLLOW_POLL_MS 200

#define USAGE \
    "dmesg [-n <lines>] [-w|--follow] [-T] [-l <level>] [--raw]\n" \
    "       -l  crit | err | warn | info | debug, or 0-7 -- that level and worse\n" \
    "       --raw  keep the <N> level marker the kernel wrote"

// A level by name or by number. Names are Linux's, shortened the way
// util-linux shortens them, so `dmesg -l err` means there what it means
// here. -1 for anything else.
static int level_by_name(const char *s) {
    if (s[0] >= '0' && s[0] <= '7' && !s[1]) return s[0] - '0';
    if (strcmp(s, "crit") == 0)  return 2;
    if (strcmp(s, "err") == 0)   return 3;
    if (strcmp(s, "warn") == 0)  return 4;
    if (strcmp(s, "info") == 0)  return 6;
    if (strcmp(s, "debug") == 0) return 7;
    return -1;
}

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
        } else if (strcmp(argv[i], "-T") == 0) {
            g_abs = 1;
        } else if (strcmp(argv[i], "--raw") == 0) {
            g_raw = 1;
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            g_min_level = level_by_name(argv[++i]);
            if (g_min_level < 0) {
                puts_("dmesg: -l wants crit, err, warn, info, debug or 0-7\n");
                return 1;
            }
        } else {
            cmd_usage(USAGE);
            return 1;
        }
    }

    if (g_abs) {
        // ONCE, before anything is printed: the boot instant is a
        // property of this run, and re-deriving it per line would let a
        // clock that ticks mid-dump disagree with itself.
        time_t now = time(NULL);
        if (now <= 0) {
            // time() reports 0 for a failed read or an RTC answering
            // year 0. Printing 1970 for every line would be a confident
            // wrong answer; say so and keep the monotonic stamps, which
            // are still true.
            puts_("dmesg: the clock is not set -- keeping monotonic times\n");
            g_abs = 0;
        } else {
            g_boot_epoch = (long long)now -
                           (long long)(sys_monotonic_ns() / 1000000000ull);
        }
    }

    int gaps = 0;
    unsigned long long next = lines ? tail_start(lines) : 0;
    next = drain(next, &gaps);
    put_flush();

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
        put_flush();
    }
}
