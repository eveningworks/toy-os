// The scratch-path builder -- see api/tmppath.h for what it is for.
//
// COMPILED TWICE, kernel and ring 3, the same rule geom.c and
// klineedit.c follow: the JOIN is identical in both rings and only
// "where does the directory come from" differs, so that half is a
// tmpdir_for() each ring supplies and this file names neither a setting
// nor a syscall.
#include "tmppath.h"
#include "kpath.h"

int tmppath(char *out, uint32_t cap, enum tmp_kind kind, const char *name) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!name || !name[0]) return 0;

    // k_path_join() already refuses rather than truncating, and already
    // handles the one-slash and trailing-slash cases -- this is the
    // toolkit call CLAUDE.md asks for instead of a fourth hand-rolled
    // concatenation. It leaves `out` alone on failure, so the clear
    // above is what makes the contract "empty on 0" true.
    if (!k_path_join(tmpdir_for(kind), name, out, cap)) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}
