// Tabs: several folders in one File Manager window, shown IN THE TITLE
// BAR -- Windows 11 Explorer's place for them, drawn by the compositor
// (win_proto.h's WIN_REQ_TABS; it reports clicks, this decides).
//
// A tab is the window's whole state as far as a person can tell: both
// panes' folders, their Back/Forward histories and which pane is
// active. Leaving a tab saves that; choosing one puts it back. The
// window's widgets are shared -- one pair of panes, refilled -- so a tab
// costs its paths and histories, not a second set of listings.
//
// A search's results are not a place (files.c saves a pane's folder the
// same way): a tab left on results comes back to the folder searched.
#include "fm_internal.h"
#include <string.h>
#include <stdio.h>
#include "kpath.h"
#include "ui/ulog.h"

#define HIST_BYTES (32 * PATH_MAX_LEN + 16)   // >= fm_history_size(), checked at init

struct tab {
    char dir[2][PATH_MAX_LEN];
    int active;
    unsigned char hist[2][HIST_BYTES];
    char label[WIN_TITLE_LEN];
};

static struct tab g_tabs[WIN_TABS_MAX];   // static: tens of KB each
static int g_ntabs, g_cur;
static char g_sent[WIN_TABS_MAX][WIN_TITLE_LEN];   // what the compositor last got
static int g_sent_n = -1, g_sent_cur = -1;

static const char *real_dir(int pane) {
    const char *d = uui_fileview_dir(&g_pane[pane]);
    const char *sc = search_scope(d);
    return sc ? sc : d;
}

static void label_of(const char *dir, char *out, int cap) {
    if (!strcmp(dir, FM_BIN)) snprintf(out, (size_t)cap, "Recycle Bin");
    else if (search_scope(dir)) snprintf(out, (size_t)cap, "Search results");
    else if (!strncmp(dir, FM_ZIP, sizeof FM_ZIP - 1)) snprintf(out, (size_t)cap, "%s", k_path_basename(dir));
    else if (dir[0] == '/' && !dir[1]) snprintf(out, (size_t)cap, "System");
    else snprintf(out, (size_t)cap, "%s", k_path_basename(dir));
}

static void save(int i) {
    struct tab *t = &g_tabs[i];
    for (int p = 0; p < 2; p++) {
        strlcpy(t->dir[p], real_dir(p), sizeof t->dir[p]);
        fm_history_save(p, t->hist[p]);
    }
    t->active = g_active;
}

static void load(int i) {
    struct tab *t = &g_tabs[i];
    for (int p = 0; p < 2; p++) {
        fm_history_load(p, t->hist[p]);
        fm_history_replay(p, t->dir[p]);
    }
    g_active = t->active;
}

// Tell the compositor -- only when what it shows would change, since
// every label is a request of its own.
void tabs_sync(struct uapp *a) {
    if (g_ntabs == 0) return;
    label_of(uui_fileview_dir(active()), g_tabs[g_cur].label, sizeof g_tabs[g_cur].label);
    int same = g_sent_n == g_ntabs && g_sent_cur == g_cur;
    for (int i = 0; same && i < g_ntabs; i++) same = !strcmp(g_sent[i], g_tabs[i].label);
    if (same) return;
    const char *labels[WIN_TABS_MAX];
    for (int i = 0; i < g_ntabs; i++) {
        labels[i] = g_tabs[i].label;
        strlcpy(g_sent[i], g_tabs[i].label, sizeof g_sent[i]);
    }
    g_sent_n = g_ntabs;
    g_sent_cur = g_cur;
    uapp_set_tabs(a, labels, g_ntabs, g_cur);
}

void tabs_init(struct uapp *a) {
    if (fm_history_size() > HIST_BYTES) {   // a history grew past the tab's room
        ulogf("files: tabs off -- history needs %d bytes\n", fm_history_size());
        return;
    }
    g_ntabs = 1;
    g_cur = 0;
    save(0);
    tabs_sync(a);
}

int tabs_count(void) { return g_ntabs; }

void tab_select(struct uapp *a, int i) {
    if (i < 0 || i >= g_ntabs || i == g_cur) return;
    save(g_cur);
    g_cur = i;
    load(i);
    ulogf("files: tab %d of %d\n", g_cur, g_ntabs);
    refresh_status();
    tabs_sync(a);
}

// A new tab on `dir` -- the active pane goes there, the other keeps the
// current tab's, as a browser's new tab keeps the window's other state.
void tab_new(struct uapp *a, const char *dir) {
    if (g_ntabs == 0) return;
    if (g_ntabs >= WIN_TABS_MAX) { set_note("no room for another tab"); return; }
    save(g_cur);
    struct tab *t = &g_tabs[g_ntabs];
    *t = g_tabs[g_cur];
    strlcpy(t->dir[t->active], dir, sizeof t->dir[0]);
    // A fresh history for the pane that moved; the other keeps its own.
    fm_history_load(t->active, 0);
    fm_history_save(t->active, t->hist[t->active]);
    g_cur = g_ntabs++;
    load(g_cur);
    fm_history_record(g_active, uui_fileview_dir(active()));
    ulogf("files: tab %d of %d\n", g_cur, g_ntabs);
    refresh_status();
    tabs_sync(a);
}

// Closing the last tab closes the window, Explorer's and every browser's
// rule.
void tab_close(struct uapp *a, int i) {
    if (i < 0 || i >= g_ntabs) return;
    if (g_ntabs == 1) { uapp_quit(a, 0); return; }
    if (i != g_cur) save(g_cur);
    memmove(&g_tabs[i], &g_tabs[i + 1], sizeof g_tabs[0] * (size_t)(g_ntabs - i - 1));
    g_ntabs--;
    int was = g_cur;
    if (i < g_cur) g_cur--;
    else if (i == g_cur) {
        if (g_cur >= g_ntabs) g_cur = g_ntabs - 1;
    }
    if (i == was) load(g_cur);
    ulogf("files: tab %d of %d\n", g_cur, g_ntabs);
    refresh_status();
    tabs_sync(a);
}

void tab_step(struct uapp *a, int delta) {
    if (g_ntabs < 2) return;
    tab_select(a, (g_cur + delta + g_ntabs) % g_ntabs);
}

int tab_current(void) { return g_cur; }
