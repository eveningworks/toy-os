// Undo and Redo for the File Manager's operations: lib/ufileundo's
// journal, what each finished operation records into it, and the toast
// that offers to take the latest back (Ctrl+Z / Ctrl+Y do the same).
#include "fm_internal.h"
#include <string.h>
#include <stdio.h>
#include "kpath.h"
#include "lib/ufileundo.h"
#include "ui/uui_toast.h"
#include "ui/uui_anim.h"
#include "ui/ulog.h"

static struct ufileundo g_undo;   // static: hundreds of KB (ufileundo.h)
struct uui_toast g_toast;
static unsigned long long g_toast_at;   // when it was shown, for its timeout
static int g_toast_redo;                // its button redoes rather than undoes

// A toast that has said its piece goes away by itself after this long --
// checked on the tick the app already runs, so it costs no timer.
#define TOAST_NS (8ull * 1000000000ull)

// The CARD look (uui_toast.h, chosen from mockups 2026-10-06): a light
// surface at the pane's corner, the operation's icon, an accent Undo and
// a dismiss slot -- a dark pill did not belong on this chrome.
static void toast(const char *icon, const char *text, const char *action, int redo) {
    uui_toast_show(&g_toast, text, uui_anim_ms(180), uui_anim_now_ns());
    uui_toast_set_action(&g_toast, action);
    g_toast.style = UUI_TOAST_CARD;
    g_toast.icon = icon;
    g_toast.closable = 1;
    g_toast_at = uui_anim_now_ns();
    g_toast_redo = redo;
}

void undo_toast_hide(void) { uui_toast_hide(&g_toast); }

int undo_toast_tick(void) {
    if (g_toast.shown && uui_anim_now_ns() - g_toast_at > TOAST_NS) {
        uui_toast_hide(&g_toast);
        return 1;
    }
    return 0;
}

static const char *folder_name(const char *dir) {
    if (!strcmp(dir, FM_BIN)) return "the Recycle Bin";
    if (dir[0] == '/' && !dir[1]) return "System";
    return k_path_basename(dir);
}

// A finished job, as the journal's entry and the toast's sentence.
void undo_record_job(int op, const char *what, char (*paths)[PATH_MAX_LEN],
                     char (*results)[PATH_MAX_LEN], int n, const char *dest) {
    (void)what;
    int kind = op == CMD_MOVE ? UFU_MOVE : op == CMD_COPY ? UFU_COPY
             : op == CMD_TRASH ? UFU_TRASH : 0;
    if (!kind) return;
    int done = 0;
    for (int i = 0; i < n; i++) if (results[i][0]) done++;
    if (!done) return;

    char label[64], one[48];
    if (done == 1) {
        for (int i = 0; i < n; i++)
            if (results[i][0]) { snprintf(one, sizeof one, "%s", k_path_basename(paths[i])); break; }
    } else {
        snprintf(one, sizeof one, "%d items", done);
    }
    if (kind == UFU_TRASH) snprintf(label, sizeof label, "Moved %s to the Recycle Bin", one);
    else snprintf(label, sizeof label, "%s %s to %s", kind == UFU_MOVE ? "Moved" : "Copied",
                  one, folder_name(dest));

    struct ufu_op *rec = ufileundo_begin(&g_undo, kind, label);
    for (int i = 0; i < n; i++) {
        if (!results[i][0]) continue;
        if (kind == UFU_TRASH) ufileundo_add(rec, paths[i], 0, results[i]);
        else ufileundo_add(rec, paths[i], results[i], 0);
    }
    ufileundo_commit(&g_undo, rec);
    ulogf("files: undo recorded \"%s\" (%d step%s)\n", label, rec->count, rec->count == 1 ? "" : "s");
    toast(kind == UFU_TRASH ? "place-trash" : kind == UFU_MOVE ? "tb-move" : "tb-copy",
          label, "Undo", 0);
}

// A rename or a new item, which the app does itself rather than as a job.
void undo_record(int kind, const char *a, const char *b) {
    char label[64];
    if (kind == UFU_RENAME) snprintf(label, sizeof label, "Renamed %s", k_path_basename(a));
    else snprintf(label, sizeof label, "Created %s", k_path_basename(b));
    struct ufu_op *rec = ufileundo_begin(&g_undo, kind, label);
    ufileundo_add(rec, a, b, 0);
    ufileundo_commit(&g_undo, rec);
    ulogf("files: undo recorded \"%s\" (1 step)\n", label);
}

int undo_can(int redo) {
    return redo ? ufileundo_next_redo(&g_undo) != 0 : ufileundo_next_undo(&g_undo) != 0;
}

void do_undo(int redo) {
    struct ufu_op *op = redo ? ufileundo_next_redo(&g_undo) : ufileundo_next_undo(&g_undo);
    if (!op) { set_note(redo ? "nothing to redo" : "nothing to undo"); return; }
    uui_toast_hide(&g_toast);
    ulogf("files: %s \"%s\"\n", redo ? "redo" : "undo", op->label);
    fm_job_undo(op, redo);
}

// The job is over: the cursor moves even when a step failed, since the
// others did run -- the failures are said, not retried.
void undo_applied(int redo, int failures) {
    struct ufu_op *op = redo ? ufileundo_next_redo(&g_undo) : ufileundo_next_undo(&g_undo);
    ufileundo_done(&g_undo, redo);
    if (!op) return;
    char text[96];
    if (failures)
        snprintf(text, sizeof text, "%s: %d could not be %s", redo ? "Redo" : "Undo",
                 failures, redo ? "redone" : "undone");
    else if (redo)
        snprintf(text, sizeof text, "%s", op->label);
    else
        snprintf(text, sizeof text, "Undone: %s", op->label);
    ulogf("files: %s done, %d failed\n", redo ? "redo" : "undo", failures);
    toast(redo ? "tb-redo" : "tb-undo", text, redo ? "Undo" : "Redo", !redo);
}

// The toast's button: whichever way the toast offers.
void undo_toast_action(void) { do_undo(g_toast_redo); }
