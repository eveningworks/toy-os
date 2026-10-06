// Recent files -- see urecent.h.
#include "lib/urecent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include "rt/sys.h"
#include "kpath.h"
#include "lib/uappentry.h"

#define TMP_FILE  URECENT_FILE ".new"
#define LOCK_FILE URECENT_FILE ".lock"

// EVERY READ AND EVERY REWRITE HOLDS A LOCK FILE: the double click that
// spawns Notepad records the file, and Notepad records it again as it
// opens, so two writers race on every open -- and a rename does not
// replace a file here, so between the old list's unlink and the new
// one's rename a reader sees no list and writes back only its own line.
// A holder takes milliseconds; one that died holding it is waited out
// for a second and then broken.
static void lock(void) {
    sys_mkdir("/var");
    sys_mkdir("/var/lib");
    for (int i = 0; i < 100; i++) {
        int fd = open(LOCK_FILE, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) { close(fd); return; }
        if (errno != EEXIST) return;   // cannot lock at all: go on unlocked
        sys_sleep_ms(10);
    }
    // Stale: its holder died. Take it over.
}

static void unlock(void) { unlink(LOCK_FILE); }

// Every line, oldest first, missing files included. Heap: a full list is
// ~30 KB, and every caller is some app's ring-3 frame.
static int read_all(struct urecent_item *out, int cap) {
    FILE *f = fopen(URECENT_FILE, "r");
    if (!f) return 0;
    int n = 0;
    char line[32 + URECENT_APP + URECENT_PATH];
    while (n < cap && fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        char *sp = strchr(line, ' '), *tab = sp ? strchr(sp, '\t') : 0;
        if (!sp || !tab || tab[1] != '/') continue;   // not a line this file holds
        *sp = *tab = '\0';
        struct urecent_item *it = &out[n++];
        it->when = strtoll(line, 0, 10);
        strlcpy(it->app, sp + 1, sizeof it->app);
        strlcpy(it->path, tab + 1, sizeof it->path);
    }
    fclose(f);
    return n;
}

static int write_all(const struct urecent_item *in, int n) {
    FILE *f = fopen(TMP_FILE, "w");
    if (!f) return -errno;
    for (int i = 0; i < n; i++) fprintf(f, "%lld %s\t%s\n", in[i].when, in[i].app, in[i].path);
    if (fclose(f) != 0) { unlink(TMP_FILE); return -EIO; }
    // A rename does not replace a file here, so the old list goes first:
    // a crash between the two leaves the new one under TMP_FILE.
    unlink(URECENT_FILE);
    return sys_rename(TMP_FILE, URECENT_FILE) == 0 ? 0 : -EIO;
}

// Drop `path` from the list, keeping the order. The new count.
static int drop(struct urecent_item *all, int n, const char *path) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(all[i].path, path)) continue;
        if (k != i) all[k] = all[i];
        k++;
    }
    return k;
}

int urecent_add(const char *path, const char *app) {
    if (!path || path[0] != '/' || strlen(path) >= URECENT_PATH || strchr(path, '\n')) return -EINVAL;
    struct urecent_item *all = malloc((URECENT_MAX + 1) * sizeof *all);
    if (!all) return -ENOMEM;
    lock();
    int n = drop(all, read_all(all, URECENT_MAX), path);
    if (n >= URECENT_MAX) {   // the oldest go
        memmove(all, all + (n - URECENT_MAX + 1), (URECENT_MAX - 1) * sizeof *all);
        n = URECENT_MAX - 1;
    }
    struct urecent_item *it = &all[n++];
    it->when = (long long)time(0);
    // The app's name is one field of a tab-separated line.
    int k = 0;
    for (const char *s = app && app[0] ? app : "?"; *s && k < URECENT_APP - 1; s++)
        it->app[k++] = (*s == '\t' || *s == '\n') ? ' ' : *s;
    it->app[k] = '\0';
    strlcpy(it->path, path, sizeof it->path);
    int e = write_all(all, n);
    unlock();
    free(all);
    return e;
}

int urecent_list(struct urecent_item *out, int cap) {
    struct urecent_item *all = malloc(URECENT_MAX * sizeof *all);
    if (!all) return 0;
    lock();
    int n = read_all(all, URECENT_MAX), k = 0;
    unlock();
    for (int i = n - 1; i >= 0 && k < cap; i--) {
        struct sys_stat st;
        if (sys_stat(all[i].path, &st) == 0) out[k++] = all[i];
    }
    free(all);
    return k;
}

int urecent_forget(const char *path) {
    struct urecent_item *all = malloc(URECENT_MAX * sizeof *all);
    if (!all) return -ENOMEM;
    lock();
    int n = read_all(all, URECENT_MAX), k = drop(all, n, path);
    int e = k == n ? -ENOENT : write_all(all, k);
    unlock();
    free(all);
    return e;
}

int urecent_clear(void) {
    lock();
    int e = write_all(0, 0);
    unlock();
    return e;
}

void urecent_app_of_exec(const char *exec, char *out, int cap) {
    struct uappentry e;
    if (uappentry_find_exec(exec, &e)) strlcpy(out, e.name, (size_t)cap);
    else strlcpy(out, k_path_basename(exec), (size_t)cap);
}
