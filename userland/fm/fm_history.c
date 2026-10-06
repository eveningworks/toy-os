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
#include "kpath.h"
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

// A TAB OWNS ITS PANES' HISTORIES (fm_tabs.c): saved when it is left,
// put back when it is chosen -- Back in a tab never goes to another
// tab's folders, as in Explorer and every browser.
int fm_history_size(void) { return (int)sizeof g_hist[0]; }
void fm_history_save(int pane, void *out) { memcpy(out, &g_hist[pane], sizeof g_hist[pane]); }
void fm_history_load(int pane, const void *in) {
    if (in) memcpy(&g_hist[pane], in, sizeof g_hist[pane]);
    else { memset(&g_hist[pane], 0, sizeof g_hist[pane]); g_hist[pane].at = -1; }
}
// Put a pane where a restored history says it is, without recording it.
void fm_history_replay(int pane, const char *dir) {
    g_replaying = 1;
    uui_fileview_set_dir(&g_pane[pane], dir);
    g_replaying = 0;
}

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

// Move by one and put the pane there. Returns 1 on arriving, 0 when
// there is nowhere to go, -1 when every entry that way has gone away.
//
// A DIRECTORY THAT HAS GONE AWAY since it was visited is STEPPED OVER,
// checked before the pane moves: set_dir() on it would leave the pane
// showing a dead path as an empty folder (ui/uui_fileview.h). The entry
// stays, because it may exist again by the time Back comes past.
static int go(int pane, int delta) {
    if (pane < 0 || pane > 1) return 0;
    struct pane_history *h = &g_hist[pane];
    int dead = -1;
    for (int want = h->at + delta; want >= 0 && want < h->count; want += delta) {
        struct sys_stat st;
        int is_bin = strcmp(h->dir[want], FM_BIN) == 0;   // always there
        if (!is_bin && (sys_stat(h->dir[want], &st) != 0 || !st.is_dir)) {
            if (dead < 0) dead = want;
            continue;
        }
        h->at = want;
        g_replaying = 1;
        uui_fileview_set_dir(&g_pane[pane], h->dir[want]);
        g_replaying = 0;
        if (dead >= 0)
            snprintf(g_stat_note, sizeof g_stat_note, "skipped %s -- it no longer exists",
                     k_path_basename(h->dir[dead]));
        return 1;
    }
    if (dead < 0) return 0;
    snprintf(g_stat_note, sizeof g_stat_note, "no such folder: %s",
             k_path_basename(h->dir[dead]));
    return -1;
}

int fm_history_back(int pane)    { return go(pane, -1); }
int fm_history_forward(int pane) { return go(pane, +1); }

// **AND THIS IS THE BACKSTOP, BECAUSE A PANE IS A WIDGET AND A WIDGET
// NAVIGATES ON ITS OWN.** `uui_fileview_release()` activates a row on a
// double click, from inside the widget, so a MOUSE descent passes
// through none of the three functions below -- exactly as Enter and
// Backspace did from inside the key handler. That bypass was fixed for
// the keyboard by comparing the directory across the call and missed
// for the mouse, which left Back dead for anyone navigating the way
// people actually navigate a file manager.
//
// So this records by RESULT rather than by route: whatever moved a
// pane, and whatever moves one in future, is caught. Safe to call as
// often as you like -- record() already ignores a repeat of the same
// place and anything during a replay -- which is what lets the caller
// be a per-frame tick instead of a list of call sites somebody has to
// keep complete.
void fm_history_sync(void) {
    for (int p = 0; p < 2; p++)
        fm_history_record(p, uui_fileview_dir(&g_pane[p]));
}

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
