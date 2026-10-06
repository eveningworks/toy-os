// Pinned folders -- see upins.h.
#include "lib/upins.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "rt/sys.h"

// Every line of the file, existing folders or not: what add and remove
// rewrite must not lose a pin to a disk that is unplugged right now.
static int read_all(char (*out)[UPINS_PATH], int cap) {
    FILE *f = fopen(UPINS_FILE, "r");
    if (!f) return 0;
    int n = 0;
    char line[UPINS_PATH + 2];
    while (n < cap && fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (line[0] != '/') continue;           // blank, a comment, or not a path
        strlcpy(out[n++], line, UPINS_PATH);
    }
    fclose(f);
    return n;
}

static int write_all(char (*in)[UPINS_PATH], int n) {
    FILE *f = fopen(UPINS_FILE, "w");
    if (!f) return -errno;
    fputs("# Folders pinned to Places (the File Manager, the file dialog)\n", f);
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", in[i]);
    return fclose(f) == 0 ? 0 : -EIO;
}

int upins_load(char (*out)[UPINS_PATH], int cap) {
    static char all[UPINS_MAX][UPINS_PATH];
    int n = read_all(all, UPINS_MAX), k = 0;
    for (int i = 0; i < n && k < cap; i++) {
        struct sys_stat st;
        if (sys_stat(all[i], &st) == 0 && st.is_dir) strlcpy(out[k++], all[i], UPINS_PATH);
    }
    return k;
}

int upins_has(const char *path) {
    static char all[UPINS_MAX][UPINS_PATH];
    int n = read_all(all, UPINS_MAX);
    for (int i = 0; i < n; i++)
        if (!strcmp(all[i], path)) return 1;
    return 0;
}

int upins_add(const char *path) {
    static char all[UPINS_MAX + 1][UPINS_PATH];
    struct sys_stat st;
    if (!path || path[0] != '/' || strlen(path) >= UPINS_PATH ||
        sys_stat(path, &st) != 0 || !st.is_dir) return -EINVAL;
    int n = read_all(all, UPINS_MAX);
    for (int i = 0; i < n; i++)
        if (!strcmp(all[i], path)) return -EEXIST;
    if (n >= UPINS_MAX) return -ENOSPC;
    strlcpy(all[n++], path, UPINS_PATH);
    return write_all(all, n);
}

int upins_remove(const char *path) {
    static char all[UPINS_MAX][UPINS_PATH];
    int n = read_all(all, UPINS_MAX), k = 0, found = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(all[i], path)) { found = 1; continue; }
        if (k != i) memcpy(all[k], all[i], UPINS_PATH);
        k++;
    }
    if (!found) return -ENOENT;
    return write_all(all, k);
}
