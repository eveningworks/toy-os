// See close_batch.h: the tracker, then the WM's one Close-all batch.
#include "close_batch.h"
#include "crash_notice.h"
#include "gui_apps.h"
#include "wm_internal.h"
#include "wm_log.h"
#include "kapi.h"
#include "rt/sys.h"   // sys_monotonic_ns()
#include <stdio.h>

void close_batch_init(struct close_batch *b, struct close_batch_entry *store, int cap) {
    b->e = store;
    b->cap = cap;
    close_batch_reset(b);
}

void close_batch_reset(struct close_batch *b) {
    b->n = 0;
    b->deadline_ns = 0;
}

int close_batch_add(struct close_batch *b, int idx) {
    if (idx < 0 || idx >= window_count || b->n >= b->cap) return 0;
    if (!windows[idx].open_seq) windows[idx].open_seq = wm_next_open_seq();
    for (int k = 0; k < b->n; k++)
        if (b->e[k].seq == windows[idx].open_seq) return 0;
    struct close_batch_entry *e = &b->e[b->n++];
    e->seq = windows[idx].open_seq;
    k_strlcpy(e->title, windows[idx].title, sizeof e->title);
    e->asked = 0;
    e->gone = 0;
    return 1;
}

int close_batch_ask(struct close_batch *b) {
    // Looked up afresh each time: a kernel-space window closes AT ONCE
    // inside wm_request_close() and renumbers the rest.
    int asked = 0;
    for (int k = 0; k < b->n; k++) {
        if (b->e[k].asked) continue;
        b->e[k].asked = 1;
        int i = close_batch_window(b, k);
        if (i >= 0) { wm_request_close(i); asked++; }
    }
    // Only a NEW ask restarts the wait: a repeated click that asks
    // nobody must not keep pushing the notice back.
    if (asked) b->deadline_ns = sys_monotonic_ns() + CLOSE_BATCH_WAIT_NS;
    return asked;
}

int close_batch_update(struct close_batch *b) {
    int changed = 0;
    for (int k = 0; k < b->n; k++)
        if (!b->e[k].gone && wm_window_by_seq(b->e[k].seq) < 0) {
            b->e[k].gone = 1;
            changed = 1;
        }
    return changed;
}

int close_batch_remaining(const struct close_batch *b) {
    int n = 0;
    for (int k = 0; k < b->n; k++) n += !b->e[k].gone;
    return n;
}

int close_batch_expired(const struct close_batch *b) {
    return b->deadline_ns && sys_monotonic_ns() >= b->deadline_ns;
}

int close_batch_window(const struct close_batch *b, int k) {
    if (k < 0 || k >= b->n || b->e[k].gone) return -1;
    return wm_window_by_seq(b->e[k].seq);
}

// --- Close all ---------------------------------------------------------------

#define CLOSE_ALL_MAX NOTICE_STAYED_MAX   // the card names every window left
static struct close_batch_entry g_store[CLOSE_ALL_MAX];
static struct close_batch g_all = { g_store, CLOSE_ALL_MAX, 0, 0 };
static char g_app_id[WIN_APP_ID_MAX];   // every window's app, or "" when they differ
static char g_icon[32];
static int g_mixed;

void close_all_begin(const int *idx, int n) {
    if (n <= 0) return;
    if (!g_all.deadline_ns) { g_all.n = 0; g_app_id[0] = 0; g_icon[0] = 0; g_mixed = 0; }
    // Recorded BEFORE any is asked: an ask can renumber windows[].
    for (int k = 0; k < n; k++) {
        int i = idx[k];
        if (i < 0 || i >= window_count) continue;
        if (!close_batch_add(&g_all, i)) continue;
        if (!g_app_id[0] && !g_mixed) {
            k_strlcpy(g_app_id, windows[i].app_id, sizeof g_app_id);
            const char *ic = wm_window_icon_name(i);
            k_strlcpy(g_icon, ic ? ic : "", sizeof g_icon);
        } else if (k_strcmp(g_app_id, windows[i].app_id) != 0) {
            g_mixed = 1;
        }
    }
    int asked = close_batch_ask(&g_all);
    wm_logf("closeall: asking %d window(s) to close\n", asked);
    redraw_pending = 1;
}

// "Notepad", from the app's own entry -- the app id is a handle, not a name.
static const char *app_name(void) {
    const struct gui_app *a = g_mixed ? 0 : gui_app_by_id(GUI_SHOW_ALL, g_app_id);
    return a ? a->name : 0;
}

// The sentence under the card's title, and its length had it fitted
// (snprintf's answer). Each title is elided to `tw` pixels; the words
// around them never are.
static int stayed_sentence(char *sub, int cap, const char *first, const char *second,
                            int left, int asking, int closed, int tw) {
    char a[CLOSE_BATCH_TITLE], b[CLOSE_BATCH_TITLE], tail[32] = "";
    ugfx_text_elide(a, sizeof a, first, tw);
    ugfx_text_elide(b, sizeof b, second ? second : "", tw);
    if (closed) snprintf(tail, sizeof tail, " The other %d closed.", closed);
    if (left == 1)
        return snprintf(sub, cap, "%s %s.%s", a,
                        asking ? "is waiting for an answer" : "did not close", tail);
    if (left == 2)
        return snprintf(sub, cap, "%s and %s did not close.%s", a, b, tail);
    return snprintf(sub, cap, "%s, %s and %d more did not close.%s", a, b, left - 2, tail);
}

static void tell_stayed(void) {
    uint32_t seq[NOTICE_STAYED_MAX];
    int n = 0, left = 0, asking = 0;
    const char *first = 0, *second = 0;
    for (int k = 0; k < g_all.n; k++) {
        if (g_all.e[k].gone) continue;
        left++;
        if (!first) first = g_all.e[k].title;
        else if (!second) second = g_all.e[k].title;
        int i = close_batch_window(&g_all, k);
        if (i >= 0 && wm_dialog_blocker(i) >= 0) asking++;
        if (n < NOTICE_STAYED_MAX) seq[n++] = g_all.e[k].seq;   // the batch holds no more
    }
    int closed = g_all.n - left;
    const char *name = app_name();
    char title[72], sub[96];
    if (name) snprintf(title, sizeof title, "%d %s window%s stayed open", left, name,
                       left == 1 ? "" : "s");
    else snprintf(title, sizeof title, "%d window%s stayed open", left, left == 1 ? "" : "s");
    // THE COUNT MUST SURVIVE: the titles give way, ONE CHARACTER of the
    // wider at a time, down to a letter and the mark, until the whole
    // sentence shows in the card's two lines.
    const char *wide = second && ugfx_text_width(second) > ugfx_text_width(first) ? second : first;
    int keep = (int)k_strlen(wide);
    for (;;) {
        int tw = keep == (int)k_strlen(wide) ? ugfx_text_width(wide)
               : ugfx_text_width_n(wide, keep) + ugfx_text_width("..");
        int len = stayed_sentence(sub, sizeof sub, first, second, left, asking, closed, tw);
        if (keep <= 1 || (len < (int)sizeof sub && crash_notice_sub_fits(sub))) break;
        keep--;
    }
    crash_notice_stayed(title, sub, g_icon[0] && !g_mixed ? g_icon : 0, seq, n);
}

void close_all_poll(void) {
    if (!g_all.deadline_ns) return;
    close_batch_update(&g_all);
    int left = close_batch_remaining(&g_all);
    if (left && !close_batch_expired(&g_all)) return;
    if (left) {
        wm_logf("closeall: %d of %d window(s) did not close\n", left, g_all.n);
        tell_stayed();
    } else {
        wm_logf("closeall: all %d window(s) closed\n", g_all.n);
    }
    close_batch_reset(&g_all);
}
