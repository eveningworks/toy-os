// opendir/readdir/closedir over SYS_LISTDIR. See <dirent.h> for why a
// DIR is a snapshot rather than a cursor.
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"

struct _DIR {
    struct sys_dirent *ents;
    int n;
    int i;
    struct dirent cur;   // what readdir() hands back a pointer to
};

DIR *opendir(const char *path) {
    if (!path) return 0;
    DIR *d = (DIR *)malloc(sizeof *d);
    if (!d) return 0;
    d->ents = (struct sys_dirent *)malloc(sizeof(struct sys_dirent) * SYS_LISTDIR_MAX);
    if (!d->ents) { free(d); return 0; }
    int n = sys_listdir(path, d->ents, SYS_LISTDIR_MAX);
    // A missing path and an empty directory are different answers and
    // must not become the same one: sys_listdir reports -1 for the
    // first and 0 for the second, and an opendir() that succeeded on a
    // missing path would make every caller's error handling dead code.
    if (n < 0) { free(d->ents); free(d); return 0; }
    d->n = n;
    d->i = 0;
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d || d->i >= d->n) return 0;
    const struct sys_dirent *e = &d->ents[d->i++];
    k_strlcpy(d->cur.d_name, e->name, sizeof d->cur.d_name);
    d->cur.d_type = e->is_dir ? DT_DIR : DT_REG;
    return &d->cur;
}

void rewinddir(DIR *d) {
    // Rewinds the SNAPSHOT, and deliberately does not re-read the
    // directory: POSIX says rewinddir resets the position, and a
    // caller that wanted the current contents would have to reopen
    // anyway (see <dirent.h>).
    if (d) d->i = 0;
}

int closedir(DIR *d) {
    if (!d) return -1;
    free(d->ents);
    free(d);
    return 0;
}
