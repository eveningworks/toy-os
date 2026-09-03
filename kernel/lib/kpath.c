// See kpath.h for the API and for why three different path resolvers
// existed before it.
//
// The normalizer is a direct descendant of shell.c's resolve_path():
// same segment-stack approach, same ".."-clamps-at-root rule, same
// depth bound. What changed is that it works on a caller-supplied
// buffer with an explicit capacity instead of assuming FS_PATH_MAX, so
// the Terminal and the shell can share it, and that it's covered by
// tests (kernel/lib/kpath_test.c) -- the original had none.
#include "kpath.h"
#include "string.h"

int k_path_is_absolute(const char *path) {
    return path && path[0] == '/';
}

int k_path_join(const char *dir, const char *name, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    if (!name) name = "";

    // An absolute `name` wins outright -- `cd /tmp` from anywhere means
    // /tmp, it doesn't mean "cwd + /tmp".
    if (k_path_is_absolute(name)) {
        if (k_strlen(name) + 1 > cap) return 0;
        k_strlcpy(out, name, cap);
        return 1;
    }

    if (!dir) dir = "";
    size_t dl = k_strlen(dir);
    // Trim trailing slashes so "/bin/" + "ls" and "/bin" + "ls" agree,
    // and so the root case doesn't produce "//ls".
    while (dl > 0 && dir[dl - 1] == '/') dl--;

    size_t nl = k_strlen(name);
    size_t need = dl + 1 + nl; // dir + '/' + name
    if (need + 1 > cap) return 0;

    for (size_t i = 0; i < dl; i++) out[i] = dir[i];
    out[dl] = '/';
    for (size_t i = 0; i < nl; i++) out[dl + 1 + i] = name[i];
    out[need] = '\0';
    return 1;
}

// `scratch` is mutated in place: each separator becomes a '\0' so every
// stack entry is its own C string pointing into it. Same trick
// shell.c's resolve_path() used.
static int normalize_in_place(char *scratch, char *out, size_t cap) {
    char *stack[KPATH_MAX_DEPTH];
    int depth = 0;

    char *p = scratch;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        size_t seglen = (size_t)(p - start);
        int had_slash = (*p == '/');
        if (had_slash) *p = '\0';

        if (seglen == 1 && start[0] == '.') {
            // "." -- nothing to do
        } else if (seglen == 2 && start[0] == '.' && start[1] == '.') {
            // ".." at the root stays at the root rather than escaping
            // above it (see kpath.h).
            if (depth > 0) depth--;
        } else {
            if (depth >= KPATH_MAX_DEPTH) return 0;
            stack[depth++] = start;
        }

        if (had_slash) p++;
    }

    if (cap < 2) return 0;
    out[0] = '/';
    out[1] = '\0';
    size_t pos = 1;
    for (int i = 0; i < depth; i++) {
        size_t seglen = k_strlen(stack[i]);
        if (i > 0) {
            if (pos + 1 + 1 > cap) return 0;
            out[pos++] = '/';
        }
        if (pos + seglen + 1 > cap) return 0;
        k_memcpy(out + pos, stack[i], seglen);
        pos += seglen;
        out[pos] = '\0';
    }
    return 1;
}

// Scratch size for the combined base+input string before collapsing.
// Callers pass paths bounded by FS_PATH_MAX (64), and a join of two is
// still far short of this -- generous on purpose, since running out
// here would mean rejecting a path that would have fit after
// normalization.
#define KPATH_SCRATCH 256

int k_path_normalize(const char *path, char *out, size_t cap) {
    if (!path || !out) return 0;
    char scratch[KPATH_SCRATCH];
    if (k_strlcpy(scratch, path, sizeof scratch) >= sizeof scratch) return 0;
    return normalize_in_place(scratch, out, cap);
}

int k_path_resolve(const char *base, const char *input, char *out, size_t cap) {
    if (!out) return 0;
    if (!base) base = "/";

    char scratch[KPATH_SCRATCH];
    if (!input || input[0] == '\0') {
        if (k_strlcpy(scratch, base, sizeof scratch) >= sizeof scratch) return 0;
    } else if (k_path_is_absolute(input)) {
        if (k_strlcpy(scratch, input, sizeof scratch) >= sizeof scratch) return 0;
    } else {
        if (!k_path_join(base, input, scratch, sizeof scratch)) return 0;
    }
    return normalize_in_place(scratch, out, cap);
}

const char *k_path_basename(const char *path) {
    if (!path) return "";
    const char *slash = k_strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int k_path_dirname(const char *path, char *out, size_t cap) {
    if (!path || !out || cap == 0) return 0;

    const char *slash = k_strrchr(path, '/');
    // No separator at all, or the only one is the leading root slash:
    // either way the containing directory is the root.
    if (!slash || slash == path) {
        if (cap < 2) return 0;
        out[0] = '/';
        out[1] = '\0';
        return 1;
    }

    size_t len = (size_t)(slash - path);
    if (len + 1 > cap) return 0;
    k_memcpy(out, path, len);
    out[len] = '\0';
    return 1;
}
