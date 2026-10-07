// lib/ulogset.h, the Log Viewer's lines: parsing, the merge, repeats,
// the filter, files and the saved form, on fixtures.
//
// WHAT A BROKEN VERSION WOULD STILL PASS, which is what shaped this:
//
//   - A merge that only worked for lines added in order passes a fixture
//     added in order; the program line goes in AFTER the kernel lines it
//     sits between.
//   - Repeat counts kept only on the first copy pass a check of the
//     first; the LAST copy is asked.
//   - A saved line that the reader cannot parse back passes a check of
//     the text alone; it is read back through ulogset_read_file() and its
//     level, source and stamp compared.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "lib/ulogset.h"
#include "lib/utest.h"

#define FILE_PATH "/tmp/ulogset_test.log"

static struct ulog_line lines[16];
static int view[16];
static struct ulogset s;

static void add(const char *src, const char *raw) { ulogset_add(&s, src, raw, (int)strlen(raw)); }

static int find(const char *text) {
    for (int i = 0; i < s.count; i++) if (!strcmp(s.lines[i].text, text)) return i;
    return -1;
}

int main(void) {
    utest_begin("ulogset_test", "the Log Viewer's line store", 0);

    ulogset_init(&s, lines, view, 16);
    add("(cut)", "ail of a line whose start the ring lost");
    add("kernel", "[1.00] usb: port 1: connected");
    add("kernel", "[3.00] <3> syscall: read() rejected -- bad fd");
    add("kernel", "[4.00] <4> wm: SLOW FRAME 1123 ms");
    add("kernel", "[5.00] <3> syscall: read() rejected -- bad fd");
    add("netd", "[2.00] netd: eth0 is now net-123456");
    add("kernel", "[6.00] <3> syscall: read() rejected -- bad fd");
    ulogset_finish(&s);

    utest_checkf(s.count == 7, "seven lines (got %d)", s.count);
    utest_check(!strcmp(s.lines[0].source, "(cut)") && !s.lines[0].stamped,
                "the ring's headless line stays first, with no time");
    utest_checkf(s.lines[1].cs == 100 && s.lines[2].cs == 200 && !strcmp(s.lines[2].source, "netd") &&
                 s.lines[3].cs == 300, "merged in time order: the program line between the kernel's (%u %u %u)",
                 s.lines[1].cs, s.lines[2].cs, s.lines[3].cs);
    int e = find("syscall: read() rejected -- bad fd");
    utest_checkf(e >= 0 && s.lines[e].level == 3 && !strcmp(s.lines[e].subsys, "syscall"),
                 "the level and the subsystem are read (level %d)", e >= 0 ? s.lines[e].level : -9);
    utest_check(s.lines[2].level == -1 && !strcmp(s.lines[2].subsys, "netd"),
                "a program line declares no level");
    utest_checkf(s.lines[6].count == 3 && s.lines[6].first == e && s.lines[6].last == 6,
                 "the LAST copy knows the count and both ends (%d, %d, %d)",
                 s.lines[6].count, s.lines[6].first, s.lines[6].last);
    utest_checkf(s.max_cs == 600, "the span ends at the latest stamp (%u)", s.max_cs);

    struct ulog_counts c;
    ulogset_counts(&s, &c);
    utest_checkf(c.errors == 3 && c.warnings == 1 && c.programs == 1, "counts: 3 errors, 1 warning, 1 program line (%d %d %d)",
                 c.errors, c.warnings, c.programs);

    struct ulog_filter f = { .only = -1 };
    f.show = ULOG_SHOW_ERRORS;
    ulogset_filter(&s, &f);
    utest_checkf(s.view_count == 3, "Errors shows the three errors (%d)", s.view_count);
    f.show = ULOG_SHOW_ALL;
    ulog_filter_mute(&f, "syscall");
    ulogset_filter(&s, &f);
    utest_checkf(s.view_count == 4, "muting syscall hides its three lines (%d)", s.view_count);
    utest_check(ulog_filter_unmute(&f, "syscall") == 0 && f.nmute == 0, "and it can be unmuted");
    snprintf(f.search, sizeof f.search, "SLOW frame");
    ulogset_filter(&s, &f);
    utest_checkf(s.view_count == 1 && s.lines[s.view[0]].level == 4, "search is case-blind (%d)", s.view_count);
    f.search[0] = 0;
    f.only = 6;
    ulogset_filter(&s, &f);
    utest_checkf(s.view_count == 3, "Only this message keeps its three copies (%d)", s.view_count);
    f.only = -1;
    f.range_lo = 150;
    f.range_hi = 450;
    ulogset_filter(&s, &f);
    utest_checkf(s.view_count == 4, "a range keeps 2.00-4.00 and the line with no time (%d)", s.view_count);
    f.range_lo = f.range_hi = 0;
    f.from = ULOG_FROM_PROGRAMS;
    ulogset_filter(&s, &f);
    utest_checkf(s.view_count == 1, "Programs keeps the one program line (%d)", s.view_count);

    // Save and read back: the saved form is logd's, so the viewer reopens it.
    int fd = open(FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char buf[256];
    for (int i = 1; i < s.count; i++) {
        int n = ulogset_format(&s, i, buf, sizeof buf);
        write(fd, buf, (size_t)n);
    }
    write(fd, "20:53:52 kernel: installed\n", 27);
    close(fd);
    static struct ulog_line lines2[16];
    static int view2[16];
    struct ulogset t;
    ulogset_init(&t, lines2, view2, 16);
    utest_check(ulogset_read_file(&t, FILE_PATH, "x.log") == 0, "the saved file reads back");
    ulogset_finish(&t);
    utest_checkf(t.count == 7, "every line came back (%d)", t.count);
    int k = -1;
    for (int i = 0; i < t.count; i++) if (!strcmp(t.lines[i].text, "netd: eth0 is now net-123456")) k = i;
    utest_checkf(k >= 0 && !strcmp(t.lines[k].source, "netd") && t.lines[k].cs == 200 && t.lines[k].level == -1,
                 "a program line keeps its source and time (%s)", k >= 0 ? t.lines[k].source : "-");
    k = -1;
    for (int i = 0; i < t.count; i++) if (t.lines[i].level == 4) k = i;
    utest_checkf(k >= 0 && !strcmp(t.lines[k].source, "kernel") && t.lines[k].cs == 400,
                 "a kernel line keeps its level, source and time (%d)", k);
    k = -1;
    for (int i = 0; i < t.count; i++) if (!strcmp(t.lines[i].clock, "20:53:52")) k = i;
    utest_checkf(k >= 0 && !strcmp(t.lines[k].text, "kernel: installed") && !strcmp(t.lines[k].source, "x.log"),
                 "a plain line keeps its clock and the file's name (%d)", k);
    unlink(FILE_PATH);

    // Full: the OLDEST go.
    ulogset_init(&s, lines, view, 4);
    for (int i = 0; i < 6; i++) {
        snprintf(buf, sizeof buf, "[%d.00] n: line %d", i, i);
        add("kernel", buf);
    }
    utest_checkf(s.count == 4 && s.dropped == 2 && !strcmp(s.lines[0].text, "n: line 2"),
                 "past its capacity the oldest are dropped (%d, %d, %s)", s.count, s.dropped, s.lines[0].text);
    return utest_end();
}
