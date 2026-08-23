// remove() and rename() -- ISO C's file-management pair.
//
// Thin by design: the kernel's own calls already return 0 or -1 with
// errno set, which is exactly what C requires, so there is nothing to
// translate. See <stdio.h> for why thin wrappers earn their place here
// when they would not elsewhere in this project.
#include <stdio.h>
#include "rt/sys.h"

// C says remove() deletes a file; on a Unix it also removes an empty
// directory, because it is unlink()-or-rmdir(). This one is unlink only,
// which is what SYS_UNLINK offers -- a directory comes back as the
// error the kernel gives rather than being silently left in place.
int remove(const char *path) { return sys_unlink(path); }

int rename(const char *oldpath, const char *newpath) {
    return sys_rename(oldpath, newpath);
}
