// log -- read the persistent log that logd keeps.
//
// `dmesg` shows the kernel's RING: what is still in memory, this boot
// only, gone when it wraps. This shows the FILE: what was persisted,
// across boots, still there after the machine has been rebooted to
// recover it. The two answer different questions and neither replaces
// the other.
//
// The file is plain text with the tag first, so everything here is a
// convenience over what `cat` and `grep` would already do -- which is
// the point. If this program is broken, the log is still readable.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "rt/sys.h"
#include "lib/cmd.h"

#define LOG_PATH "/var/log/toyos.log"
#define LOG_PREV "/var/log/toyos.log.1"

static const char *USAGE =
    "log [-n <lines>] [-u <tag>] [-l <level>] [-p] [-f] [--raw]\n"
    "       -n  show only the last <lines>       -u  only lines from <tag>\n"
    "       -l  crit|err|warn|info|debug, or 0-7 -- that level and worse\n"
    "       -p  the PREVIOUS boot's log          -f  follow as it grows\n"
    "       --raw  keep the <N> level marker the kernel wrote";

// --raw, and -l. A KERNEL line carries "<N> " after its stamp (the
// level is text, see kernel/lib/klog.c); an application line carries
// none, because a program declares no level. A line with no marker is
// never filtered out -- the alternative is `-l err` silently hiding
// every service's output.
static int g_raw;
static int g_min_level = 7;

static int level_by_name(const char *s) {
    if (s[0] >= '0' && s[0] <= '7' && !s[1]) return s[0] - '0';
    if (strcmp(s, "crit") == 0)  return 2;
    if (strcmp(s, "err") == 0)   return 3;
    if (strcmp(s, "warn") == 0)  return 4;
    if (strcmp(s, "info") == 0)  return 6;
    if (strcmp(s, "debug") == 0) return 7;
    return -1;
}

// Where "<N> " sits in this line, or -1. Searched rather than computed
// from a fixed offset: the tag is padded to one width and the stamp is
// not, so the marker's column moves with the uptime.
#define MARKER_SEARCH 40
static int marker_at(const char *line) {
    for (int i = 0; i < MARKER_SEARCH && line[i]; i++)
        if (line[i] == '<' && line[i + 1] >= '0' && line[i + 1] <= '7' &&
            line[i + 2] == '>' && line[i + 3] == ' ')
            return i;
    return -1;
}

// Prints one line, dropping it if its level is below the filter and
// hiding the marker unless --raw asked for it.
static void emit_line(const char *line) {
    int at = marker_at(line);
    if (at < 0) { printf("%s\n", line); return; }
    if (line[at + 1] - '0' > g_min_level) return;
    if (g_raw) { printf("%s\n", line); return; }
    printf("%.*s%s\n", at, line, line + at + 4);
}

// The tag sits at a fixed offset because logd writes it first and
// pads it, so this is an exact comparison rather than a substring
// search that a MESSAGE mentioning netd would also satisfy.
static int tag_is(const char *line, const char *want) {
    if (line[0] != '[') return 0;
    const char *end = strchr(line, ']');
    if (!end) return 0;
    size_t n = strlen(want);
    if ((size_t)(end - line - 1) < n) return 0;
    if (strncmp(line + 1, want, n) != 0) return 0;
    // Everything after the name must be the padding, not another name.
    for (const char *p = line + 1 + n; p < end; p++)
        if (*p != ' ') return 0;
    return 1;
}

static char g_buf[8192];

// Streamed, never slurped: a log is exactly the file that outgrows any
// buffer somebody sized for it, and reading it whole to print it would
// fail on the one that matters most.
static int show(const char *path, const char *tag, int last_n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "log: no %s yet\n", path);
        return 1;
    }
    // -n is a TAIL, so it needs the end of a file whose length is not
    // known until it is read. Two passes over the file rather than one
    // buffer big enough for it: the second pass prints from the line
    // the first pass counted back to.
    long total = 0;
    if (last_n > 0) {
        int n;
        while ((n = (int)read(fd, g_buf, sizeof g_buf)) > 0)
            for (int i = 0; i < n; i++) if (g_buf[i] == '\n') total++;
        lseek(fd, 0, SEEK_SET);
    }
    long skip = (last_n > 0 && total > last_n) ? total - last_n : 0;

    char line[600];
    unsigned len = 0;
    long seen = 0;
    int n;
    while ((n = (int)read(fd, g_buf, sizeof g_buf)) > 0) {
        for (int i = 0; i < n; i++) {
            if (g_buf[i] != '\n') {
                if (len < sizeof line - 1) line[len++] = g_buf[i];
                continue;
            }
            line[len] = '\0';
            if (seen++ >= skip && (!tag || tag_is(line, tag)))
                emit_line(line);
            len = 0;
        }
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    const char *tag = 0;
    const char *path = LOG_PATH;
    int last_n = 0, follow = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) { tag = argv[++i]; }
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) { last_n = atoi(argv[++i]); }
        else if (strcmp(argv[i], "-p") == 0) { path = LOG_PREV; }
        else if (strcmp(argv[i], "-f") == 0) { follow = 1; }
        else if (strcmp(argv[i], "--raw") == 0) { g_raw = 1; }
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            g_min_level = level_by_name(argv[++i]);
            if (g_min_level < 0) {
                fprintf(stderr, "log: -l wants crit, err, warn, info, debug or 0-7\n");
                return 1;
            }
        }
        else { cmd_usage(USAGE); return 1; }
    }

    if (!follow) return show(path, tag, last_n);

    // FOLLOW IS A POLL, not a notification: there is no inotify here and
    // the file grows from another process, so the only honest way to
    // watch it is to look again. It never returns; Ctrl-C ends it.
    long off = 0;
    // A partial line survives the poll: the file grows mid-line, and a
    // marker split across two reads would be neither hidden nor matched.
    static char line[600];
    unsigned len = 0;
    int per_line = g_raw || g_min_level < 7 || tag;
    for (;;) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            lseek(fd, off, SEEK_SET);
            int n;
            while ((n = (int)read(fd, g_buf, sizeof g_buf)) > 0) {
                off += n;
                if (!per_line) { write(STDOUT_FILENO, g_buf, (unsigned)n); continue; }
                for (int i = 0; i < n; i++) {
                    if (g_buf[i] != '\n') {
                        if (len < sizeof line - 1) line[len++] = g_buf[i];
                        continue;
                    }
                    line[len] = '\0';
                    if (!tag || tag_is(line, tag)) emit_line(line);
                    len = 0;
                }
                fflush(stdout);
            }
            close(fd);
        }
        sys_sleep_ms(500);
    }
}
