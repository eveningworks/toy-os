#ifndef ULIB_UTEST_H
#define ULIB_UTEST_H

// The harness every self-checking /tests program reports through: a
// banner, one line per check, and ONE epilogue line in the shape
// tools/usertest_run.py knows by default --
//
//     <name>: <title>
//     <name>: ok   <what>
//     <name>: FAIL <what> -- <detail>
//     <name>: all checks passed (N checks)      exit 0
//     <name>: FAILED -- N of M checks           exit N
//
// Every line carries the test's name, because a spawned test's output
// lands in the kernel log between everything else the machine says,
// and the runner scopes its "FAIL" search to lines that name the test.
//
// IT WRITES WITH sys_write(), NEVER THROUGH stdio. A test of the stream
// layer (stdio_test) or of the C library as a whole must not report
// through the thing under test: a broken fputs would take the FAIL
// line with it. The formatting variant does go through vsnprintf,
// which is kfmt's formatter and not the stream layer.
//
// A header rather than a .c for cmd.h's reason: libuapp.a is on every
// test's link line, and a program that includes this links only what
// it calls. Not thread-safe -- report from the main thread.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "tmppath.h"
#include "lib/utmppath.h"

enum {
    // Tee every line to /tmp/<name>.out, which is what usertest_run.py
    // reads for a SPAWNED test: `spawn` returns as soon as the child
    // exists, and the child's console output arrives while the harness
    // is between commands, where it is dropped. Streamed, not written at
    // the end, so a test that dies mid-way leaves the lines it reached.
    UTEST_VERDICT_FILE = 1,
    // Report on fd 2 -- the kernel log -- instead of stdout. For a test
    // a KTEST spawns with no terminal, whose report is read from `dmesg`.
    UTEST_KLOG = 2,
    // Print failures only. For a table of hundreds of cases, where the
    // epilogue's count is the evidence and 400 "ok" lines are noise.
    UTEST_QUIET = 4,
};

static const char *utest_name;
static int utest_flags;
static int utest_verdict_fd = -1;
static int utest_checks;
static int utest_fails;

static inline void utest_emit(const char *s) {
    size_t n = strlen(s);
    sys_write((utest_flags & UTEST_KLOG) ? 2 : 1, s, n);
    if (utest_verdict_fd >= 0) sys_write(utest_verdict_fd, s, n);
}

// "<name>: <text>\n"
static inline void utest_line(const char *text) {
    utest_emit(utest_name);
    utest_emit(": ");
    utest_emit(text);
    utest_emit("\n");
}

static inline void utest_begin(const char *name, const char *title, int flags) {
    utest_name = name;
    utest_flags = flags;
    utest_checks = utest_fails = 0;
    if (flags & UTEST_VERDICT_FILE) {
        // TMP_VOLATILE: the harness reads this back in the same boot and
        // never after it.
        char base[64], path[64];
        snprintf(base, sizeof base, "%s.out", name);
        if (!tmppath(path, sizeof path, TMP_VOLATILE, base)) return;
        // Best effort: with the scratch directory unwritable, or the
        // name too long to fit under it, the console copy still stands.
        utest_verdict_fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    }
    if (title) utest_line(title);
}

static inline void utest_check(int ok, const char *what) {
    utest_checks++;
    if (!ok) utest_fails++;
    if (ok && (utest_flags & UTEST_QUIET)) return;
    utest_emit(utest_name);
    utest_emit(ok ? ": ok   " : ": FAIL ");
    utest_emit(what);
    utest_emit("\n");
}

// The detail -- what was got, what was wanted -- appears only on a
// failure; a passing line stays short.
static inline void utest_check_detail(int ok, const char *what, const char *detail) {
    if (ok || !detail || !detail[0]) { utest_check(ok, what); return; }
    char line[256];
    snprintf(line, sizeof line, "%s -- %s", what, detail);
    utest_check(0, line);
}

__attribute__((format(printf, 2, 3)))
static inline void utest_checkf(int ok, const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    utest_check(ok, line);
}

// A line that is not a check: a measurement, what a child said.
__attribute__((format(printf, 1, 2)))
static inline void utest_notef(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    utest_line(line);
}

static inline int utest_failed(void) { return utest_fails; }

// The whole test stands down, as a KTEST does on a fixture the image
// was built without. Exit 0: a red table would train people to ignore it.
static inline int utest_skip(const char *why) {
    char line[160];
    snprintf(line, sizeof line, "SKIP %s", why);
    utest_line(line);
    if (utest_verdict_fd >= 0) sys_close(utest_verdict_fd);
    return 0;
}

// Returns the exit code: the number of failed checks. Zero checks is a
// failure, not a pass -- an emptied table must not read as green.
static inline int utest_end(void) {
    char line[96];
    if (utest_checks == 0) {
        utest_fails = 1;
        utest_line("FAILED -- no checks ran");
    } else if (utest_fails) {
        snprintf(line, sizeof line, "FAILED -- %d of %d checks", utest_fails, utest_checks);
        utest_line(line);
    } else {
        snprintf(line, sizeof line, "all checks passed (%d checks)", utest_checks);
        utest_line(line);
    }
    if (utest_verdict_fd >= 0) sys_close(utest_verdict_fd);
    return utest_fails > 255 ? 255 : utest_fails;
}

#endif
