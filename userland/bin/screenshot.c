// `screenshot` -- write a picture of the screen to a file.
//
// An ORDINARY program with no window, which is the whole point: it asks
// the compositor for pixels over the client channel (lib/ushot.h), so it
// works from a terminal window, from the physical console with the
// desktop running, and over telnet on a machine whose screen nobody is
// looking at. That last one is what makes a bare-metal capture possible
// at all -- tools/remote.py runs this and fetches the file.
//
// The Screenshot app is the other front end on the same library; neither
// wraps the other.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/ushot.h"

#define USAGE "screenshot [-w] [-r x,y,w,h] [-p] [-d seconds] [-f qoi|png] [path]"

#define SHOT_DIR "/home/screenshots"

static int parse_region(const char *s, int *x, int *y, int *w, int *h) {
    // "x,y,w,h", all four required. A partial rectangle is refused
    // rather than completed with guesses, the parser rule this project
    // follows everywhere.
    const char *p = s;
    int *out[4] = { x, y, w, h };
    for (int i = 0; i < 4; i++) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) return 0;
        *out[i] = (int)v;
        p = end;
        if (i < 3) {
            if (*p != ',') return 0;
            p++;
        }
    }
    return *p == '\0';
}

// /home/screenshots/shot-YYYYMMDD-HHMMSS.<ext>, and the directory is
// made on demand -- it is not seeded, because an empty directory nobody
// has taken a screenshot into is clutter.
static void default_path(char *buf, size_t cap, const char *ext) {
    sys_mkdir(SHOT_DIR);
    time_t now = time(NULL);
    struct tm tm;
    char stamp[32];
    if (localtime_r(&now, &tm) && strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm))
        snprintf(buf, cap, SHOT_DIR "/shot-%s.%s", stamp, ext);
    else
        snprintf(buf, cap, SHOT_DIR "/shot-%d.%s", sys_getpid(), ext);
}

int main(int argc, char **argv) {
    int mode = WIN_SHOT_SCREEN, delay = 0;
    unsigned flags = 0;
    int rx = 0, ry = 0, rw = 0, rh = 0;
    const char *format = NULL;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-w") == 0 || strcmp(a, "--window") == 0) {
            mode = WIN_SHOT_WINDOW;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--pointer") == 0) {
            flags |= WIN_SHOT_POINTER;
        } else if ((strcmp(a, "-r") == 0 || strcmp(a, "--region") == 0) && i + 1 < argc) {
            if (!parse_region(argv[++i], &rx, &ry, &rw, &rh)) {
                printf("screenshot: a region is x,y,w,h\n");
                return 1;
            }
            mode = WIN_SHOT_REGION;
        } else if ((strcmp(a, "-d") == 0 || strcmp(a, "--delay") == 0) && i + 1 < argc) {
            delay = atoi(argv[++i]);
        } else if ((strcmp(a, "-f") == 0 || strcmp(a, "--format") == 0) && i + 1 < argc) {
            format = argv[++i];
            if (strcmp(format, "qoi") != 0 && strcmp(format, "png") != 0) {
                printf("screenshot: the formats are qoi and png\n");
                return 1;
            }
        } else if (a[0] == '-' && a[1]) {
            cmd_usage(USAGE);
            return 1;
        } else if (!path) {
            path = a;
        } else {
            cmd_usage(USAGE);
            return 1;
        }
    }

    char generated[96];
    if (!path) {
        default_path(generated, sizeof generated, format ? format : "qoi");
        path = generated;
    }

    // THE DELAY IS BEFORE THE CONNECT, not just before the capture: a
    // delay exists so the person can go and arrange the screen, and a
    // program that had already opened a channel would be one more thing
    // running while they did it.
    if (delay > 0) sleep((unsigned)delay);

    struct ushot s;
    int rc = ushot_open(&s);
    if (rc < 0) {
        printf("screenshot: %s\n", ushot_strerror(rc));
        return 1;
    }

    rc = ushot_take(&s, mode, flags, rx, ry, rw, rh);
    if (rc == 0) rc = ushot_save(&s, path, format);
    int w = s.w, h = s.h;
    ushot_close(&s);

    if (rc < 0) {
        printf("screenshot: %s\n", ushot_strerror(rc));
        return 1;
    }

    // Silent when the caller named the file, because it already knows
    // where it is; the generated name is information it has no other way
    // to learn. Same rule `mktemp` follows.
    if (path == generated) printf("%s (%dx%d)\n", path, w, h);
    return 0;
}
