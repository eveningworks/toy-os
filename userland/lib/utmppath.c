// tmpdir_for() for ring 3 -- the other half of the seam
// kernel/lib/tmppath.c leaves open (api/tmppath.h).
//
// CACHED, and the reason is not speed. Every path syscall in a program
// would otherwise carry a SYS_SETTING round trip, and a program that
// builds a path inside a loop would make one per iteration. The cost of
// caching is that a setting changed while a program runs is not seen by
// that program -- which is the right answer anyway: a process that
// picked /tmp for one file and /scratch for the next would leave half
// its work somewhere nobody looks.
#include "tmppath.h"
#include "utmppath.h"
#include "usetting.h"
#include <string.h>

static char g_dir[2][64];
static int  g_loaded[2];

static const char *default_for(enum tmp_kind kind) {
    return kind == TMP_PERSISTENT ? TMP_VARDIR_DEFAULT : TMP_DIR_DEFAULT;
}

const char *tmpdir_for(enum tmp_kind kind) {
    int i = (kind == TMP_PERSISTENT) ? 1 : 0;
    if (!g_loaded[i]) {
        g_loaded[i] = 1;
        const char *name = i ? "storage.vartmpdir" : "storage.tmpdir";
        char v[64];
        // A MISSING OR UNREADABLE SETTING IS NOT AN ERROR HERE. A
        // program started before the registry exists -- or on a build
        // with no /etc at all -- still needs somewhere to put a file,
        // and the compiled default is the same one the kernel's layout
        // pass created. Anything not absolute is refused rather than
        // used, since a relative scratch dir would resolve against
        // whatever the process's cwd happened to be.
        if (usetting_get(name, v, sizeof v) && v[0] == '/') {
            strncpy(g_dir[i], v, sizeof g_dir[i] - 1);
        } else {
            strncpy(g_dir[i], default_for(kind), sizeof g_dir[i] - 1);
        }
    }
    return g_dir[i];
}

const char *utest_path(enum tmp_kind kind, const char *name) {
    static char bufs[4][96];
    static unsigned turn;
    char *b = bufs[turn++ & 3u];
    if (!tmppath(b, sizeof bufs[0], kind, name)) b[0] = '\0';
    return b;
}
