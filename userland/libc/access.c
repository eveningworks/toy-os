// access() -- see <unistd.h> for why F_OK is the only meaningful mode.
#include <unistd.h>
#include <limits.h>
#include <fs.h>
#include "rt/sys.h"

// <limits.h> spells PATH_MAX out rather than including this header,
// because it is pulled in by nearly everything and must stay
// dependency-free. This is where the two are held together.
_Static_assert(PATH_MAX == FS_PATH_MAX, "PATH_MAX and FS_PATH_MAX disagree");

int access(const char *path, int mode) {
    (void)mode;
    struct sys_stat st;
    return sys_stat(path, &st) == 0 ? 0 : -1;
}
