// trash -- the Recycle Bin from a prompt: move files into it, list it,
// put one back, or empty it. The same bins and the same lib/utrash as
// the File Manager's Delete, so either sees what the other did --
// `gio trash` and trash-cli are this command on a Linux desktop.
//
// `rm` stays permanent. This is the reversible delete, asked for by name.
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "lib/uargs.h"
#include "lib/utrash.h"
#include "lib/ufileop.h"
#include "lib/human.h"
#include "lib/udate.h"
#include "kpath.h"
#include <errno.h>

static const struct uargs_cmd CMDS[] = {
    { "put",     "FILE...", "move each FILE into its volume's bin" },
    { "list",    0,         "what is in the bins, newest first" },
    { "restore", "NAME...", "put an item back where it was deleted from" },
    { "empty",   0,         "delete everything in the bins for good" },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "trash",
    .usage = "COMMAND [ARG]...",
    .summary = "Move files to the Recycle Bin, list it, restore from it, or empty it.",
    .cmds = CMDS,
    .notes = "NAME is the first column of `trash list`: the item's name in its bin,\n"
             "which gains a number (dusk.2.jpg) when the bin already held one.\n"
             "A file on a RAM or read-only volume has no bin; `rm` deletes it.",
};

// Static: struct ufileop is tens of KB and the list is bigger still.
static struct ufileop g_op;
static struct utrash_item g_items[256];

static void op_error(void *ctx, const char *path, int err) {
    (void)ctx;
    fprintf(stderr, "trash: %s: %s\n", path, sys_strerror(err));
}

static int put(int argc, char **argv) {
    int rc = 0;
    for (int i = 0; i < argc; i++) {
        struct utrash_item it;
        int e = utrash_put(argv[i], &it);
        if (e == -ENOTSUP) {
            fprintf(stderr, "trash: %s: this volume has no bin (rm deletes it)\n", argv[i]);
            rc = 1;
        } else if (e) {
            fprintf(stderr, "trash: %s: %s\n", argv[i], sys_strerror(-e));
            rc = 1;
        } else if (strcmp(it.name, k_path_basename(argv[i]))) {
            printf("%s -> %s\n", argv[i], it.name);
        }
    }
    return rc;
}

static int list(void) {
    int n = utrash_list(g_items, 256);
    for (int i = 0; i < n; i++) {
        char when[48], size[24];
        udate_format(when, sizeof when, &g_items[i].deleted, UDATE_DATE | UDATE_TIME);
        if (g_items[i].is_dir) strlcpy(size, "folder", sizeof size);
        else human_size(size, sizeof size, g_items[i].size);
        printf("%-24s %9s  %-16s  %s\n", g_items[i].name, size, when, g_items[i].orig);
    }
    if (!n) printf("the Recycle Bin is empty\n");
    return 0;
}

static int restore(int argc, char **argv) {
    int n = utrash_list(g_items, 256), rc = 0;
    for (int a = 0; a < argc; a++) {
        int found = 0;
        for (int i = 0; i < n && !found; i++) {
            if (strcmp(g_items[i].name, argv[a])) continue;
            found = 1;
            int e = utrash_restore(&g_items[i]);
            if (e == -EEXIST)
                fprintf(stderr, "trash: %s: something named that is in the way\n", g_items[i].orig);
            else if (e == -ENOENT)
                fprintf(stderr, "trash: %s: the folder it came from is gone\n", g_items[i].orig);
            else if (e)
                fprintf(stderr, "trash: %s: %s\n", argv[a], sys_strerror(-e));
            else
                printf("%s\n", g_items[i].orig);
            if (e) rc = 1;
        }
        if (!found) { fprintf(stderr, "trash: %s: not in the bin\n", argv[a]); rc = 1; }
    }
    return rc;
}

static int empty(void) {
    struct ufileop_policy pol = { .on_error = op_error };
    int n = utrash_list(g_items, 256), rc = 0;
    for (int i = 0; i < n; i++) {
        char p[UTRASH_PATH + 80];
        if (!utrash_item_path(&g_items[i], p, sizeof p)) continue;
        if (ufileop_remove(p, 1, &g_op, &pol) != UFILEOP_OK) { rc = 1; continue; }
        if (utrash_forget(&g_items[i])) rc = 1;
    }
    printf("%d item%s deleted\n", n, n == 1 ? "" : "s");
    return rc;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc < 1) return uargs_error(&PROG, "a command: put, list, restore or empty");
    const char *c = a.argv[0];
    if (!strcmp(c, "put") || !strcmp(c, "restore")) {
        if (a.argc < 2) return uargs_error(&PROG, "%s needs at least one name", c);
        return c[0] == 'p' ? put(a.argc - 1, a.argv + 1) : restore(a.argc - 1, a.argv + 1);
    }
    if (a.argc > 1) return uargs_error(&PROG, "%s takes no arguments", c);
    return !strcmp(c, "list") ? list() : empty();
}
