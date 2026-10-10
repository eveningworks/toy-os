// thumb -- make thumbnails in the shared cache, /var/cache/thumbnails.
//
// THE DESKTOP'S THUMBNAILER. The desktop is drawn by the compositor,
// which must never run a picture or video decoder on someone else's
// file: it spawns this instead and reads back only the finished QOI
// (lib/uthumb.h), Windows' out-of-process thumbnail handlers' shape. A
// file that crashes a decoder takes this program with it, not the
// desktop. Anything else may run it too -- it fills the File Manager's
// cache ahead of a visit.
//
// Exit status: 0 every FILE has a thumbnail, 1 some FILE is not a
// picture or a video (or cannot be read), 2 a usage error.
#include <stdio.h>
#include <stdlib.h>
#include "lib/uargs.h"
#include "lib/uthumb.h"

static const char *g_size;
static int g_quiet;

static const struct uargs_opt OPTS[] = {
    { "size", 's', "PX", "the thumbnail's longer side in pixels, 16..256 (default 64)", 0, &g_size },
    { "quiet", 'q', 0, "print nothing; the exit status says how it went", &g_quiet, 0 },
    { 0 }
};

static const struct uargs_prog PROG = {
    .name = "thumb",
    .usage = "[-s PX] [-q] FILE...",
    .summary = "Make each FILE's thumbnail in /var/cache/thumbnails, or confirm the one\n"
               "there is newer than the file. Pictures (QOI, PNG, JPEG, BMP, GIF) and videos.",
    .opts = OPTS,
    .notes = "Exit status: 0 every FILE has one, 1 some FILE is not a picture or a video.",
};

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    int px = g_size ? atoi(g_size) : 64;
    if (px < 16 || px > 256) return uargs_error(&PROG, "a size of %s is not 16..256", g_size);
    if (a.argc < 1) return uargs_error(&PROG, "no FILE");
    int bad = 0;
    for (int i = 0; i < a.argc; i++) {
        int rc = uthumb_make(a.argv[i], px);
        if (rc) bad = 1;
        if (!g_quiet) printf("%s: %s\n", a.argv[i], rc ? "not a picture or a video" : "ok");
    }
    return bad;
}
