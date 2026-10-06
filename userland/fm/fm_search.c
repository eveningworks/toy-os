// Search into subfolders: the "search:<folder>" virtual folder (a
// uui_fileview source) and the worker that fills it. Typing in the
// search box still FILTERS the folder you are in (files.c's
// pane_filter); Enter asks this to walk the tree below it, the way
// Dolphin's filter bar and its "From here (including subfolders)"
// search sit side by side. There is no index to ask, so the walk is
// the search -- on a thread of its own, posting as it finds things,
// with Stop.
#include "fm_internal.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <pthread.h>
#include "kpath.h"
#include "lib/uwalk.h"
#include <caltime.h>
#include "lib/human.h"
#include "lib/udate.h"
#include "ui/uui_table.h"
#include "ui/ulog.h"

#define SEARCH_MAX PANE_FILES

struct hit {
    char path[PATH_MAX_LEN];
    struct sys_dirent e;
};

// WRITTEN BY THE WORKER under g_lock; read by the main thread under it.
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hit g_hits[SEARCH_MAX];
static int g_nhits;
static int g_full;                        // there were more than SEARCH_MAX
static unsigned long long g_seen;         // entries looked at
static char g_where[PATH_MAX_LEN];        // the directory being read, for the strip

static char g_root[PATH_MAX_LEN], g_query[UUI_TEXTBOX_MAX];
static volatile int g_running, g_cancel;
static pthread_t g_thread;
static struct uwalk g_walk;               // static: several KB
static unsigned long long g_last_post;

// What the listing last copied out, so `path` and `cell` answer for the
// rows the pane holds even while the worker keeps adding.
static struct hit g_shown[SEARCH_MAX];
static int g_nshown;

static int matches(const char *name) {
    int qn = (int)strlen(g_query), n = (int)strlen(name);
    for (int i = 0; i + qn <= n; i++)
        if (!strncasecmp(name + i, g_query, (size_t)qn)) return 1;
    return 0;
}

static int visit(void *ctx, const char *path, const struct sys_dirent *e) {
    (void)ctx;
    if (g_cancel) return -1;
    // Hidden names the way the panes hide them -- and a bin is never
    // searched: what is in it was deleted.
    if (e->name[0] == '.' && (!g_opt.hidden || !strncmp(e->name, ".Trash", 6))) return 0;
    if (matches(e->name)) {
        pthread_mutex_lock(&g_lock);
        if (g_nhits < SEARCH_MAX) {
            strlcpy(g_hits[g_nhits].path, path, PATH_MAX_LEN);
            g_hits[g_nhits].e = *e;
            g_nhits++;
        } else {
            g_full = 1;
        }
        pthread_mutex_unlock(&g_lock);
    }
    return 1;
}

static void *worker(void *arg) {
    (void)arg;
    if (uwalk_begin(&g_walk, g_root)) {
        while (!g_cancel && uwalk_step(&g_walk, visit, 0)) {
            pthread_mutex_lock(&g_lock);
            g_seen = g_walk.entries;
            if (g_walk.depth > 0) strlcpy(g_where, g_walk.frame[g_walk.depth - 1].path, sizeof g_where);
            pthread_mutex_unlock(&g_lock);
            // Posted at most every ~150 ms: a post per directory would
            // be thousands of events for one search.
            unsigned long long now = sys_monotonic_ns();
            if (now - g_last_post > 150000000ull) {
                g_last_post = now;
                uapp_post(g_app, POST_SEARCH, 0);
            }
        }
    }
    pthread_mutex_lock(&g_lock);
    g_seen = g_walk.entries;
    pthread_mutex_unlock(&g_lock);
    g_running = 0;
    uapp_post(g_app, POST_SEARCH, 1);
    return 0;
}

void search_stop(void) {
    if (!g_running) return;
    g_cancel = 1;
    // The walk checks the flag per entry, so this is short; joined so a
    // new search never shares the state with an old one.
    pthread_join(g_thread, 0);
    g_running = 0;
}

void search_start(const char *root, const char *query) {
    search_stop();
    pthread_mutex_lock(&g_lock);
    g_nhits = g_full = 0;
    g_seen = 0;
    g_where[0] = '\0';
    pthread_mutex_unlock(&g_lock);
    strlcpy(g_root, root, sizeof g_root);
    strlcpy(g_query, query, sizeof g_query);
    g_cancel = 0;
    g_running = 1;
    g_last_post = 0;
    ulogf("files: search start \"%s\" under %s\n", g_query, g_root);
    if (pthread_create(&g_thread, 0, worker, 0) != 0) {
        g_running = 0;
        set_note("could not start the search");
    }
}

int search_running(void) { return g_running; }

// The strip's words: what is happening, or what came of it.
void search_status(char *out, int cap) {
    pthread_mutex_lock(&g_lock);
    int n = g_nhits, full = g_full;
    unsigned long long seen = g_seen;
    pthread_mutex_unlock(&g_lock);
    if (g_running)
        snprintf(out, (size_t)cap, "Searching... %d found, %llu looked at", n, seen);
    else if (full)
        snprintf(out, (size_t)cap, "The first %d found, of %llu looked at", n, seen);
    else
        snprintf(out, (size_t)cap, "%d found, %llu looked at", n, seen);
}

// --- the virtual folder ---------------------------------------------------

static const struct uui_table_column search_cols[] = {
    { "Name",     0,  UUI_TALIGN_LEFT  },
    { "Size",     9,  UUI_TALIGN_RIGHT },
    { "Modified", 20, UUI_TALIGN_LEFT  },
    { "Folder",   20, UUI_TALIGN_LEFT  },
};

static int s_list(void *ctx, const char *dir, struct sys_dirent *out, int cap) {
    (void)ctx; (void)dir;
    pthread_mutex_lock(&g_lock);
    g_nshown = g_nhits < cap ? g_nhits : cap;
    memcpy(g_shown, g_hits, sizeof g_shown[0] * (size_t)g_nshown);
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < g_nshown; i++) out[i] = g_shown[i].e;
    return g_nshown;
}

static int s_path(void *ctx, int i, char *out, int cap) {
    (void)ctx;
    return i >= 0 && i < g_nshown && (int)strlcpy(out, g_shown[i].path, (size_t)cap) < cap;
}

static void s_cell(void *ctx, int i, int col, char *out, int cap) {
    (void)ctx;
    out[0] = '\0';
    if (i < 0 || i >= g_nshown) return;
    const struct sys_dirent *e = &g_shown[i].e;
    if (col == 1 && !e->is_dir) human_size(out, (unsigned long)cap, e->size);
    else if (col == 2) udate_format(out, (unsigned long)cap, &e->modified, UDATE_DATE | UDATE_TIME);
    else if (col == 3) k_path_dirname(g_shown[i].path, out, (size_t)cap);
}

// By the value, not the text: "9K" sorts after "10K" as a string.
static int s_compare(void *ctx, int a, int b, int col) {
    (void)ctx;
    const struct hit *x = &g_shown[a], *y = &g_shown[b];
    if (col == 1) return x->e.size < y->e.size ? -1 : x->e.size > y->e.size;
    if (col == 2) {
        uint64_t tx = cal_rtc_to_epoch(&x->e.modified), ty = cal_rtc_to_epoch(&y->e.modified);
        return tx < ty ? -1 : tx > ty;
    }
    return strcmp(x->path, y->path);
}

static const struct uui_fileview_source g_src = {
    .list = s_list, .path = s_path,
    .cols = search_cols, .ncols = 4,
    .cell = s_cell, .compare = s_compare,
};

const struct uui_fileview_source *search_source(void) { return &g_src; }

int in_search(const struct uui_fileview *fv) { return uui_fileview_source(fv) == &g_src; }

// "search:/usr" -> "/usr": the folder a search view searches.
const char *search_scope(const char *dir) {
    return strncmp(dir, FM_SEARCH, sizeof FM_SEARCH - 1) == 0 ? dir + sizeof FM_SEARCH - 1 : 0;
}

// Nothing is created, pasted or renamed in a list of results: they live
// in many folders, and "here" is none of them.
int search_item_flags(int code, unsigned *out) {
    if (!in_search(active())) return 0;
    switch (code) {
    case CMD_MENU_NEW: case CMD_MKDIR: case CMD_NEW_FILE: case CMD_CLIP_PASTE:
    case CMD_RENAME: case CMD_MOVE: case CMD_COPY:
        *out = UUI_MI_DISABLED;
        return 1;
    default:
        return 0;
    }
}
