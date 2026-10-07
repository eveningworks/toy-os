// runask -- the "Run backup.sh?" card as a program of its own, for
// callers with no window to hang it on: the desktop's double-click and
// `open` from a shell both reach it through lib/uopen.c. Windows'
// OpenWith.exe is the same arrangement. The File Manager shows the same
// card in-process (ui/uui_runask.h).
//
//   runask <path>
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/uui_runask.h"
#include <stdio.h>

static void done(void *ctx, const char *path, int kind, int act) {
    (void)ctx;
    if (act == ULAUNCH_ASK) return;
    // Not tracked: this program exits next, and the child outlives it.
    if (uui_runask_start(0, path, kind, act) < 0) ulogf("runask: could not start %s\n", path);
}

int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] == '-') {
        fprintf(stderr, "usage: runask <path>\n");
        return 1;
    }
    struct ulaunch_info info;
    if (!ulaunch_classify(argv[1], &info) || info.kind == ULAUNCH_NONE) {
        fprintf(stderr, "runask: %s is not a program or a script\n", argv[1]);
        return 1;
    }
    struct uapp_desc desc = { 0 };
    uui_runask_app_desc(&desc, argv[1], &info, done, 0);
    return uapp_run(&desc);
}
