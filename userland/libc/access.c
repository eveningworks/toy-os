// access() -- see <unistd.h> for why F_OK is the only meaningful mode.
#include <unistd.h>
#include "rt/sys.h"

int access(const char *path, int mode) {
    (void)mode;
    struct sys_stat st;
    return sys_stat(path, &st) == 0 ? 0 : -1;
}
