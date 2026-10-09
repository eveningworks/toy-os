// lib/utrace: strace's records as text, from records built here -- no
// trace, no kernel, so every branch of the decoder is reachable. What
// the formatter KTESTs checked while the kernel formatted, plus the two
// things only a reader of records can get wrong: pairing an exit with
// its entry, and the bytes the kernel copied.
#include <stdio.h>
#include <string.h>
#include "syscall_abi.h"
#include "errno.h"
#include "trace_abi.h"
#include "lib/utrace.h"
#include "lib/utest.h"

static struct trace_rec entry(int nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    struct trace_rec r;
    memset(&r, 0, sizeof r);
    r.nr = (uint16_t)nr;
    r.kind = TRACE_ENTRY;
    r.pid = 7;
    r.a[0] = a0; r.a[1] = a1; r.a[2] = a2;
    r.blob_arg = 0xFF;
    return r;
}

static struct trace_rec leave(int nr, int kind, int64_t ret) {
    struct trace_rec r;
    memset(&r, 0, sizeof r);
    r.nr = (uint16_t)nr;
    r.kind = (uint16_t)kind;
    r.pid = 7;
    r.ret = ret;
    r.blob_arg = 0xFF;
    return r;
}

static void blob(struct trace_rec *r, int arg, const char *s, int len, int cut) {
    memcpy(r->blob, s, (size_t)len);
    r->blob_len = (uint16_t)len;
    r->blob_arg = (uint8_t)arg;
    r->blob_cut = (uint8_t)cut;
}

static char g_lines[8][400];
static int g_n;
static void collect(void *ctx, const char *line) {
    (void)ctx;
    if (g_n < 8) snprintf(g_lines[g_n++], sizeof g_lines[0], "%s", line);
}

static void call_is(const struct trace_rec *e, const char *want, const char *what) {
    char got[256];
    utrace_format_call(got, sizeof got, e);
    utest_checkf(!strcmp(got, want), "%s -- got %s", what, got);
}

static void ret_is(int nr, const struct trace_rec *x, const char *want, const char *what) {
    char got[64];
    utrace_format_ret(got, sizeof got, nr, x);
    utest_checkf(!strcmp(got, want), "%s -- got '%s'", what, got);
}

int main(void) {
    utest_begin("utrace_test", "strace's records as text", 0);

    // --- names and arguments --------------------------------------
    struct trace_rec e = entry(SYS_OPEN, 0x1000, SYS_O_WRITE | SYS_O_CREAT, 0);
    blob(&e, 0, "notes.txt", 9, 0);
    call_is(&e, "open(\"notes.txt\", O_WRITE|O_CREAT)", "a path and its flags, by name");
    e = entry(SYS_OPEN, 0x1000, SYS_O_WRITE | 0x4000, 0);
    call_is(&e, "open(0x1000, O_WRITE|0x4000)",
            "a path the kernel could not read is its pointer; an unknown flag bit stays");
    e = entry(SYS_WRITE, 1, 0x2000, 7);
    blob(&e, 1, "hi\n\t\"\\\x01", 7, 0);
    call_is(&e, "write(1, \"hi\\n\\t\\\"\\\\\\x01\", 7)",
            "a buffer is escaped as strace escapes it");
    e = entry(SYS_WRITE, 1, 0x2000, 4096);
    blob(&e, 1, "abc", 3, 1);
    call_is(&e, "write(1, \"abc\"..., 4096)", "a buffer the record cut short says so");
    e = entry(SYS_WRITE, 1, 0x2000, 0);
    blob(&e, 1, "", 0, 0);
    call_is(&e, "write(1, \"\", 0)", "an empty buffer is \"\", not a pointer");
    e = entry(999, 1, 2, 3);
    call_is(&e, "syscall_999(0x1, 0x2, 0x3)", "a number with no row still traces, in hex");
    utest_check(utrace_lookup("open") == SYS_OPEN && utrace_lookup("nosuch") == -1,
                "a name finds its number, and only a real name does");

    // --- return values ---------------------------------------------
    struct trace_rec x = leave(SYS_OPEN, TRACE_EXIT, 3);
    ret_is(SYS_OPEN, &x, " = 3", "a count is decimal");
    x = leave(SYS_OPEN, TRACE_EXIT, -ENOENT);
    ret_is(SYS_OPEN, &x, " = -2 ENOENT", "an error is its number and its name");
    utest_check(utrace_failed(&x), "...and counts as a failure");
    x = leave(SYS_OPEN, TRACE_EXIT, -4000);
    ret_is(SYS_OPEN, &x, " = -4000", "an errno with no name is still shown");
    x = leave(SYS_SBRK, TRACE_EXIT, 0x8010000000);
    ret_is(SYS_SBRK, &x, " = 0x8010000000", "a pointer is hex");
    x = leave(SYS_SBRK, TRACE_EXIT, -ENOMEM);
    ret_is(SYS_SBRK, &x, " = -12 ENOMEM", "...but a pointer call's error is an error");
    x = leave(SYS_EXIT, TRACE_NORETURN, 0);
    ret_is(SYS_EXIT, &x, " = ?", "a call that never returns says so");

    // --- pairing -----------------------------------------------------
    struct utrace_printer p;
    utrace_printer_init(&p, collect, 0);
    e = entry(SYS_GETPID, 0, 0, 0);
    utrace_feed(&p, &e);
    x = leave(SYS_GETPID, TRACE_EXIT, 7);
    utrace_feed(&p, &x);
    e = entry(SYS_WAITPID, 23, 0x30, 0);
    utrace_feed(&p, &e);
    e = entry(SYS_GETPID, 0, 0, 0);
    e.pid = 8;                          // another thread, in between
    utrace_feed(&p, &e);
    x = leave(SYS_GETPID, TRACE_EXIT, 7);
    x.pid = 8;
    utrace_feed(&p, &x);
    x = leave(SYS_WAITPID, TRACE_RESUMED, 23);
    utrace_feed(&p, &x);
    e = entry(SYS_EXIT, 0, 0, 0);
    utrace_feed(&p, &e);
    utrace_flush(&p);
    utest_checkf(g_n == 5, "five lines from four calls and a dangling entry (%d)", g_n);
    utest_check(g_n > 0 && !strcmp(g_lines[0], "getpid() = 7"), "an entry and its exit are one line");
    utest_check(g_n > 1 && !strcmp(g_lines[1], "waitpid(23, 0x30, 0) <unfinished ...>"),
                "an entry interrupted by another call is unfinished");
    utest_check(g_n > 3 && !strcmp(g_lines[3], "<... waitpid resumed> = 23"),
                "...and its exit, later, resumed with its value");
    utest_check(g_n > 4 && !strcmp(g_lines[4], "exit(0) <unfinished ...>"),
                "an entry with no exit at the end is flushed as unfinished");

    // --- a full line never overruns ----------------------------------
    char small[16];
    memset(small, 0x7f, sizeof small);
    e = entry(SYS_OPEN, 0x1000, 0, 0);
    blob(&e, 0, "a-rather-long-path-name", 23, 0);
    size_t n = utrace_format_call(small, 8, &e);
    int intact = 1;
    for (unsigned i = 8; i < sizeof small; i++) if (small[i] != 0x7f) intact = 0;
    utest_check(n < 8 && small[n] == 0 && intact, "a short buffer truncates and terminates in place");
    return utest_end();
}
