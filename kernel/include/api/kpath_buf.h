#ifndef KPATH_BUF_H
#define KPATH_BUF_H

#include <stddef.h>
#include "kpath.h"

// A path buffer that is not on the kernel stack.
//
// FS_PATH_MAX is 4096 and a kernel stack is 16 KiB with one guard page,
// so a function holding two path locals spends half its stack and a
// third one steps over the guard into unmapped space. Linux has the
// same arithmetic and the same answer: `getname()` takes a path from a
// slab (`names_cachep`) and `putname()` returns it, and no path is ever
// a local. These are that pair.
//
// **RING 0 ONLY.** api/ is on ring 3's include path as well, but the
// implementation calls kmalloc, so a ring-3 caller gets a link error.
// In ring 3 declare the buffer or malloc it -- an 8 MiB stack makes
// that affordable, which is the whole reason this file is not shared.
//
// A get() can FAIL, and every caller therefore grows an error path
// (-ENOMEM); that is the cost of the move off the stack and it is
// deliberate, since the alternative is a fixed pool that fails at a
// depth nobody can predict.
char *kpath_get(void);
void  kpath_put(char *buf);

// A scratch big enough to resolve two FS_PATH_MAX paths against each
// other, for k_path_resolve()/k_path_normalize(). Separate from
// kpath_get() because it is TWICE the size -- a resolve joins before
// it collapses, so the working string outgrows its own result.
int  kpath_scratch_get(struct kpath_scratch *out);
void kpath_scratch_put(struct kpath_scratch *s);

#endif
