// getenv/setenv/unsetenv/putenv -- the C API over libsys's `environ`.
//
// `environ` itself lives in userland/rt/sys.c, not here: libsys owns
// the process's startup vector, because crt0 is libsys and argc, argv
// and envp arrive together. This file is the part C specifies.
//
// **THE OWNERSHIP RULE, which is where every environment implementation
// gets subtle.** The array crt0 hands over points into the INITIAL
// STACK -- memory this library did not allocate and must never free.
// The moment anything is added or changed, the whole array moves to the
// heap and a flag records that, so free() is only ever called on
// entries this file put there. glibc carries the same distinction and
// the same flag; skipping it frees stack addresses on the second
// setenv().
//
// putenv() is the exception that keeps the rule visible: it stores the
// CALLER's string rather than a copy, so that string must outlive the
// call and must not be freed by us. C says so, and it is why setenv()
// exists as the one that copies.
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"

// 1 once `environ` and every entry in it are ours to free.
static int g_owned;
static int g_count;     // entries, excluding the NULL
static int g_capacity;  // slots allocated, excluding the NULL

// Length of the name part, i.e. up to '='. A variable with no '=' is
// malformed; callers below reject rather than guess at its name.
static size_t name_len(const char *entry) {
    size_t n = 0;
    while (entry[n] && entry[n] != '=') n++;
    return n;
}

static int matches(const char *entry, const char *name, size_t nlen) {
    return entry[nlen] == '=' && k_strncmp(entry, name, nlen) == 0;
}

char *getenv(const char *name) {
    if (!name || !*name || !environ) return 0;
    size_t nlen = k_strlen(name);
    for (int i = 0; environ[i]; i++)
        if (matches(environ[i], name, nlen)) return environ[i] + nlen + 1;
    return 0;
}

// Moves the environment onto the heap the first time it is modified,
// copying every entry. Until then it is the stack image crt0 gave us
// and nothing in it may be freed.
static int take_ownership(void) {
    if (g_owned) return 1;
    int n = 0;
    if (environ) while (environ[n]) n++;
    int cap = n + 8;   // room for a few additions before the next grow
    char **fresh = (char **)malloc((size_t)(cap + 1) * sizeof(char *));
    if (!fresh) return 0;
    for (int i = 0; i < n; i++) {
        fresh[i] = strdup(environ[i]);
        if (!fresh[i]) {
            // Unwind rather than leave a half-copied environment: a
            // partially owned array is the state nothing else here
            // knows how to reason about.
            for (int k = 0; k < i; k++) free(fresh[k]);
            free(fresh);
            return 0;
        }
    }
    fresh[n] = 0;
    environ = fresh;
    g_owned = 1;
    g_count = n;
    g_capacity = cap;
    return 1;
}

static int grow(void) {
    int cap = g_capacity * 2 + 8;
    char **bigger = (char **)realloc(environ, (size_t)(cap + 1) * sizeof(char *));
    if (!bigger) return 0;
    environ = bigger;
    g_capacity = cap;
    return 1;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || k_strchr(name, '=') || !value) { errno = EINVAL; return -1; }
    if (!take_ownership()) { errno = ENOMEM; return -1; }

    size_t nlen = k_strlen(name);
    size_t vlen = k_strlen(value);

    for (int i = 0; i < g_count; i++) {
        if (!matches(environ[i], name, nlen)) continue;
        if (!overwrite) return 0;   // present, and the caller said leave it
        char *entry = (char *)malloc(nlen + vlen + 2);
        if (!entry) { errno = ENOMEM; return -1; }
        k_memcpy(entry, name, nlen);
        entry[nlen] = '=';
        k_memcpy(entry + nlen + 1, value, vlen + 1);
        // Replace, then free -- in that order, so `environ` is never
        // momentarily pointing at freed memory.
        char *old = environ[i];
        environ[i] = entry;
        free(old);
        return 0;
    }

    if (g_count >= g_capacity && !grow()) { errno = ENOMEM; return -1; }
    char *entry = (char *)malloc(nlen + vlen + 2);
    if (!entry) { errno = ENOMEM; return -1; }
    k_memcpy(entry, name, nlen);
    entry[nlen] = '=';
    k_memcpy(entry + nlen + 1, value, vlen + 1);
    environ[g_count++] = entry;
    environ[g_count] = 0;
    return 0;
}

int unsetenv(const char *name) {
    if (!name || !*name || k_strchr(name, '=')) { errno = EINVAL; return -1; }
    if (!environ) return 0;
    if (!take_ownership()) { errno = ENOMEM; return -1; }
    size_t nlen = k_strlen(name);
    for (int i = 0; i < g_count; i++) {
        if (!matches(environ[i], name, nlen)) continue;
        free(environ[i]);
        // Move the LAST entry into the gap rather than shuffling every
        // one after it down. C promises nothing about the order of
        // environ, and this is O(1) instead of O(n).
        environ[i] = environ[--g_count];
        environ[g_count] = 0;
        return 0;
    }
    return 0;   // absent is not an error, as C says
}

int putenv(char *entry) {
    // STORES THE CALLER'S POINTER, no copy -- C's actual contract, and
    // the reason setenv() exists. A caller passing a stack buffer here
    // leaves the environment pointing at a dead frame the moment that
    // function returns.
    if (!entry || !k_strchr(entry, '=')) { errno = EINVAL; return -1; }
    if (!take_ownership()) { errno = ENOMEM; return -1; }
    size_t nlen = name_len(entry);
    for (int i = 0; i < g_count; i++) {
        if (!matches(environ[i], entry, nlen)) continue;
        free(environ[i]);
        environ[i] = entry;
        return 0;
    }
    if (g_count >= g_capacity && !grow()) { errno = ENOMEM; return -1; }
    environ[g_count++] = entry;
    environ[g_count] = 0;
    return 0;
}

int clearenv(void) {
    if (!take_ownership()) { errno = ENOMEM; return -1; }
    for (int i = 0; i < g_count; i++) free(environ[i]);
    g_count = 0;
    environ[0] = 0;
    return 0;
}
