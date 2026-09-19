// Per-pane navigation history -- back and forward.
//
// **PER PANE, NOT PER APP.** Each pane is its own place, the way each
// tab is in Explorer and Dolphin; a shared stack would send the left
// pane back to somewhere the RIGHT pane had been, which reads as the
// window losing its mind rather than as a feature.
//
// THE SHAPE IS THE BROWSER'S, and it is the shape because everyone
// expects it: going somewhere new TRUNCATES the forward list. The
// alternative -- keeping it, so forward still goes to the old branch --
// is a tree, which no file manager or browser offers because nobody can
// hold the second branch in their head.
//
// A BOUNDED RING, oldest dropped. HISTORY_MAX is deliberately small:
// this is "where was I", not an audit trail, and a person who has
// visited 32 directories does not want the 33rd-from-last back.
#include "fm_internal.h"
#include "ui/uui_fileview.h"
#include <string.h>
#include <stdio.h>

#define HISTORY_MAX 32

struct pane_history {
    char dir[HISTORY_MAX][PATH_MAX_LEN];
    int count;    // entries in use
    int at;       // where we are, 0..count-1; -1 before the first record
};

static struct pane_history g_hist[2];

// Set while history itself is driving the pane, so the set_dir() that
// results does not record a new entry on top of the one it is
// replaying -- which would make Back push Back onto the stack and the
// button would never reach anything older.
static int g_replaying;

void fm_history_record(int pane, const char *dir) {
    if (pane < 0 || pane > 1 || !dir || !dir[0] || g_replaying) return;
    struct pane_history *h = &g_hist[pane];

    // THE SAME PLACE TWICE IS NOT A STEP. A refresh, or activating the
    // directory you are already in, must not fill the history with
    // entries Back cannot move past.
    if (h->at >= 0 && strcmp(h->dir[h->at], dir) == 0) return;

    if (h->at + 1 < HISTORY_MAX) {
        h->at++;
    } else {
        // Full: drop the oldest and keep the newest. The cost is that
        // the far end of Back quietly gets shorter, which is what every
        // bounded history does.
        memmove(h->dir[0], h->dir[1], sizeof h->dir[0] * (HISTORY_MAX - 1));
    }
    snprintf(h->dir[h->at], PATH_MAX_LEN, "%s", dir);
    h->count = h->at + 1;   // anything ahead is now unreachable
}

int fm_history_can_back(int pane)    { return pane >= 0 && pane <= 1 && g_hist[pane].at > 0; }
int fm_history_can_forward(int pane) {
    return pane >= 0 && pane <= 1 && g_hist[pane].at + 1 < g_hist[pane].count;
}

// Move by one and put the pane there. Returns 0 when there is nowhere
// to go, so a caller can say so rather than silently doing nothing.
static int go(int pane, int delta) {
    if (pane < 0 || pane > 1) return 0;
    struct pane_history *h = &g_hist[pane];
    int want = h->at + delta;
    if (want < 0 || want >= h->count) return 0;
    h->at = want;
    g_replaying = 1;
    int ok = uui_fileview_set_dir(&g_pane[pane], h->dir[want]);
    g_replaying = 0;
    // A DIRECTORY THAT HAS GONE AWAY since it was visited leaves the
    // pane empty (ui/uui_fileview.h). Stepping back over it rather than
    // stranding the user there is what a browser does with a dead tab
    // restore; the entry stays, because going the other way may still
    // work.
    return ok;
}

int fm_history_back(int pane)    { return go(pane, -1); }
int fm_history_forward(int pane) { return go(pane, +1); }

// THE ONE PLACE A PANE'S DIRECTORY CHANGES. It used to change at four
// call sites with nothing in common, which is exactly how a history
// ends up describing somewhere the user has never been -- so the
// recording lives here rather than at each of them, and a fifth call
// site cannot forget.
int fm_goto(int pane, const char *dir) {
    if (pane < 0 || pane > 1) return 0;
    if (!uui_fileview_set_dir(&g_pane[pane], dir)) return 0;
    fm_history_record(pane, uui_fileview_dir(&g_pane[pane]));
    return 1;
}

// The same, for the two moves the fileview makes on its own -- it
// resolves the destination itself, so the record is taken AFTER from
// wherever it landed rather than from a path computed here.
int fm_goto_up(int pane) {
    if (pane < 0 || pane > 1) return 0;
    uui_fileview_up(&g_pane[pane]);
    fm_history_record(pane, uui_fileview_dir(&g_pane[pane]));
    return 1;
}

int fm_goto_activate(int pane) {
    if (pane < 0 || pane > 1) return 0;
    if (!uui_fileview_activate(&g_pane[pane])) return 0;
    fm_history_record(pane, uui_fileview_dir(&g_pane[pane]));
    return 1;
}
