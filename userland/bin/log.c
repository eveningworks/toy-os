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
    "log [-n <lines>] [-u <tag>] [-p] [-f]\n"
    "       -n  show only the last <lines>       -u  only lines from <tag>\n"
    "       -p  the PREVIOUS boot's log          -f  follow as it grows";

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
                printf("%s\n", line);
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
        else { cmd_usage(USAGE); return 1; }
    }

    if (!follow) return show(path, tag, last_n);

    // FOLLOW IS A POLL, not a notification: there is no inotify here and
    // the file grows from another process, so the only honest way to
    // watch it is to look again. It never returns; Ctrl-C ends it.
    long off = 0;
    for (;;) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            lseek(fd, off, SEEK_SET);
            int n;
            while ((n = (int)read(fd, g_buf, sizeof g_buf)) > 0) {
                write(STDOUT_FILENO, g_buf, (unsigned)n);
                off += n;
            }
            close(fd);
        }
        sys_sleep_ms(500);
    }
}
