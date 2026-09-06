// logd -- the persistent log, /var/log/toyos.log.
//
// WHY THIS EXISTS, and it is not a hypothetical. The kernel ring holds a
// few hundred lines. Twice in one day an intermittent fault destroyed
// its own evidence: a driver polling a device that had gone away logged
// a line a second and flushed a laptop's entire boot log, and the boot
// that mattered could not be diagnosed at all. A ring is the wrong place
// to keep something you will want after the fact.
//
// WHAT IT IS NOT. Not journald: there is no index, no binary store and
// no query language. The file is text, one line each, because every
// breakthrough in that investigation came from reading raw log text with
// `grep` -- and a format only one tool can read would have cost exactly
// the post-mortem this exists to make possible. `cat` is a working
// reader when `log` is broken, which is the property worth protecting.
//
// THE TAG COMES FIRST, then whatever the source said, verbatim:
//
//     [kernel] [0.90] usb: port 2: connected, low-speed
//
// Verbatim because reformatting is lossy and the kernel's boot-relative
// stamp is the most reliable clock this machine has early on, before
// anything has set the time. Tag first and fixed-width so `log -u
// kernel` matches at a fixed offset rather than hunting for a substring
// that a MESSAGE might also contain.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "rt/sys.h"
#include "lib/usetting.h"
#include "lib/utmppath.h"
#include "query_abi.h"

#define LOG_PATH   "/var/log/toyos.log"
#define LOG_PREV   "/var/log/toyos.log.1"
#define TAG_W      6                  // "kernel", "toywm ", "netd  "
#define POLL_MS    1000

// How far into the kernel's byte stream we have persisted. Absolute,
// counted from the first byte ever logged -- NOT a ring position, which
// moves under a reader (api/klog.h). Comparing it against the `first`
// the kernel reports is how a gap becomes visible instead of silent.
static unsigned long long g_seen;

static int g_fd = -1;
static unsigned long long g_written;   // bytes in the current file

static void emit(const char *tag, const char *line, unsigned len) {
    if (g_fd < 0 || !len) return;
    char pre[TAG_W + 4];
    int n = snprintf(pre, sizeof pre, "[%-*s] ", TAG_W, tag);
    if (n > 0) { write(g_fd, pre, (unsigned)n); g_written += (unsigned)n; }
    write(g_fd, line, len);
    write(g_fd, "\n", 1);
    g_written += len + 1;
}

// The cap is read EACH TIME rather than cached: lowering it on a machine
// that is filling up should take effect without restarting the daemon.
static unsigned long long cap_bytes(void) {
    int mib = 0;
    if (!usetting_get_int("storage.log_max", &mib)) mib = 4;
    return (unsigned long long)mib * 1024 * 1024;
}

// TWO FILES, and the older one is what answers "what did the last boot
// say" -- which is the question asked after a machine has been rebooted
// to recover it, and the reason one file would not do.
static void rotate_if_needed(void) {
    unsigned long long cap = cap_bytes();
    if (!cap) return;
    if (g_written < cap / 2) return;   // half each, so the pair fits the cap
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    unlink(LOG_PREV);
    rename(LOG_PATH, LOG_PREV);
    g_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    g_written = 0;
}

// Split the kernel's byte stream into lines. A read can end mid-line, so
// the tail is held until its newline arrives rather than emitted as a
// short line -- a log that splits messages at arbitrary points is worse
// than one that lags a second.
static char g_line[512];
static unsigned g_line_len;

static void feed(const char *p, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (p[i] == '\n') {
            emit("kernel", g_line, g_line_len);
            g_line_len = 0;
        } else if (g_line_len < sizeof g_line) {
            g_line[g_line_len++] = p[i];
        }
        // A line longer than the buffer is TRUNCATED, not wrapped: the
        // kernel does not emit any, and silently splitting one would
        // invent a second message that was never logged.
    }
}

// One pass over whatever is new. Returns 0 when caught up.
static int drain(void) {
    struct query_klog r;
    if (sys_query_record(QUERY_KLOG, 0, &r, sizeof r) <= 0) return 0;

    if (g_seen < r.first) {
        // BYTES AGED OUT BEFORE WE GOT THEM, and saying so is the whole
        // point of tracking an absolute offset: a gap the reader cannot
        // see is indistinguishable from a quiet machine.
        char note[96];
        int n = snprintf(note, sizeof note,
                         "logd: %llu byte(s) lost before they were persisted",
                         (unsigned long long)(r.first - g_seen));
        if (n > 0) emit("logd", note, (unsigned)n);
        g_seen = r.first;
    }
    if (g_seen >= r.total) return 0;

    unsigned long long off = g_seen - r.first;
    unsigned idx = (unsigned)(off / QUERY_KLOG_DATA);
    unsigned skip = (unsigned)(off % QUERY_KLOG_DATA);
    if (sys_query_record(QUERY_KLOG, idx, &r, sizeof r) <= 0) return 0;
    if (skip >= r.len) return 0;

    feed((const char *)r.data + skip, r.len - skip);
    g_seen += r.len - skip;
    return 1;
}

int main(void) {
    if (!cap_bytes()) {
        // 0 means the maintainer asked for no logging. Exiting cleanly
        // is the honest answer -- init reports a one-shot that returned
        // 0 as `done`, and a daemon that stayed up doing nothing would
        // read as working.
        sys_eprint("logd: storage.log_max is 0 -- logging disabled\n");
        return 0;
    }

    // The PREVIOUS file first: this boot's log belongs beside the last
    // one, not on top of it.
    unlink(LOG_PREV);
    rename(LOG_PATH, LOG_PREV);
    g_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (g_fd < 0) { sys_eprint("logd: cannot open " LOG_PATH "\n"); return 1; }

    // FROM THE OLDEST BYTE STILL RETAINED, not from now: everything the
    // kernel logged before this daemon started is exactly the part
    // worth keeping, and it is still in the ring at this point.
    struct query_klog r;
    if (sys_query_record(QUERY_KLOG, 0, &r, sizeof r) > 0) g_seen = r.first;

    for (;;) {
        while (drain()) { }
        rotate_if_needed();
        sys_sleep_ms(POLL_MS);
    }
}
