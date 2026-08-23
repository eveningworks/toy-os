// mkdir() -- <sys/stat.h>'s one function, over SYS_MKDIR.
#include <sys/stat.h>
#include "rt/sys.h"

int mkdir(const char *path, mode_t mode) {
    // Read and discarded rather than left unnamed: this filesystem has
    // no permission bits, so there is nothing for the mode to mean. See
    // the header for why that is an ignore and not a dishonour.
    (void)mode;
    // sys_mkdir() already returns 0 or -1 with sys_errno() set, which is
    // exactly POSIX's contract -- there is nothing to translate.
    return sys_mkdir(path);
}
