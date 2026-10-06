// The name prompt: Rename, New folder and New text file when Options
// says "in a dialog" -- the card dialog (ui/uui_dialog.h) with a text
// field as its body. The in-place rename is the fileview's own
// (uui_fileview_begin_rename) and does not come here.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "ui/uui_renamer.h"
#include "ui/ulog.h"
#include "kpath.h"
#include <stdio.h>
#include "ui/uui_dialog.h"
#include "ui/utheme.h"
#include "lib/icon_cache.h"
#include "lib/ufiletype.h"
#include <string.h>
#include <stdio.h>

// Up while the prompt is. Kept as a flag of its own because the input
// code gates on "is something modal up" in several places.
enum modal_kind g_modal;
static struct uui_textbox g_field;
static struct uui_item g_field_item = { .ops = &uui_textbox_ops, .widget = &g_field,
                                        .name = "prompt" };
static int g_modal_cmd;          // what to do when it commits
static char g_modal_row[UUI_FILEVIEW_PATH_MAX + 24];
static const char *const g_modal_rows[] = { g_modal_row };

void open_prompt(int cmd, const char *title, const char *initial) {
    g_modal = MODAL_PROMPT;
    g_modal_cmd = cmd;
    g_dialog_kind = DIALOG_PROMPT;

    uui_textbox_init(&g_field, initial ? initial : "");
    uui_textbox_set_active(&g_field, 1);
    // The name, not the extension (a rename); everything else starts
    // selected so typing replaces it.
    int n = (int)strlen(g_field.buf), stem = n;
    const char *dot = strrchr(g_field.buf, '.');
    if (cmd != CMD_MKDIR && dot && dot != g_field.buf) stem = (int)(dot - g_field.buf);
    uui_textbox_select(&g_field, 0, stem);

    // What it is about, under the title: the file being renamed, or
    // where the new one goes.
    const char *about = uui_fileview_dir(active());
    if (cmd == CMD_RENAME)
        snprintf(g_modal_row, sizeof g_modal_row, "%s -- %s", initial,
                 ufiletype_name(initial, uui_fileview_selected_is_dir(active())));
    else
        snprintf(g_modal_row, sizeof g_modal_row, "in %s", about);

    static const struct uui_dialog_button rename_btns[] = {
        { "Cancel", DLG_CANCEL, 0 }, { "Rename", DLG_OK, UUI_DLG_PRIMARY },
    };
    static const struct uui_dialog_button create_btns[] = {
        { "Cancel", DLG_CANCEL, 0 }, { "Create", DLG_OK, UUI_DLG_PRIMARY },
    };
    uui_dialog_open(&g_dialog, title, g_modal_rows, 1,
                    cmd == CMD_RENAME ? rename_btns : create_btns, 2, 1, DLG_CANCEL);
    int fh;
    uui_textbox_natural_size(&g_field, 0, &fh);
    uui_dialog_set_body(&g_dialog, &g_field_item, ugfx_char_w() * 30, fh);
    uui_dialog_focus(&g_dialog, &g_field_item);

    const char *icon = cmd == CMD_MKDIR ? "folder"
                     : cmd == CMD_NEW_FILE ? "file-text"
                     : ufiletype_icon(initial, uui_fileview_selected_is_dir(active()));
    // The file's own picture when a thumbnail is ready, as on the delete
    // card; its type's icon otherwise.
    const struct uimg *pic = 0;
    const struct sys_dirent *e = cmd == CMD_RENAME ? uui_fileview_selected_entry(active()) : 0;
    if (e && !e->is_dir && g_opt.thumbs)
        pic = pane_thumb(0, uui_fileview_dir(active()), e, ugfx_char_h() * 4);
    uui_dialog_set_picture(&g_dialog, pic ? pic : icon_get(icon, ugfx_char_h() * 4));
}

// The dialog answered. Called from files.c's answer_dialog() with the
// button's code; the field is read before it is taken down.
void answer_prompt(int code) {
    int cmd = g_modal_cmd;
    char text[UUI_TEXTBOX_MAX];
    strlcpy(text, uui_textbox_text(&g_field), sizeof text);
    g_modal = MODAL_NONE;
    uui_dialog_set_body(&g_dialog, NULL, 0, 0);
    if (code != DLG_OK) { set_note("cancelled"); return; }
    if (cmd == CMD_MKDIR) commit_mkdir(text);
    else if (cmd == CMD_RENAME) commit_rename(text);
    else if (cmd == CMD_NEW_FILE) commit_newfile(text);
}

// --- rename many (ui/uui_renamer.h) ----------------------------------------
//
// THROUGH TEMPORARY NAMES, in two passes: every item first to a name of
// its own that nothing has, then each to its new name. A set whose new
// names include another's old one -- photo-02 and photo-01 numbered the
// other way round -- would otherwise fail on the first rename.
static void rename_many_done(void *ctx, const char *dir, char (*olds)[URENAME_NAME],
                             char (*news)[URENAME_NAME], int n) {
    (void)ctx;
    static char tmp[UUI_RENAMER_MAX][URENAME_NAME];
    char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
    int moved = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        snprintf(tmp[i], URENAME_NAME, ".rename-%d-%s", i, olds[i]);
        if (!k_path_join(dir, olds[i], a, sizeof a) || !k_path_join(dir, tmp[i], b, sizeof b) ||
            sys_rename(a, b) != 0) { failed = 1; break; }
        moved++;
    }
    if (failed) {   // put back what moved, and touch nothing more
        for (int i = 0; i < moved; i++)
            if (k_path_join(dir, tmp[i], a, sizeof a) && k_path_join(dir, olds[i], b, sizeof b))
                sys_rename(a, b);
        set_note("could not rename them -- nothing was changed");
        reload_panes();
        return;
    }
    int done = 0;
    for (int i = 0; i < n; i++) {
        if (k_path_join(dir, tmp[i], a, sizeof a) && k_path_join(dir, news[i], b, sizeof b) &&
            sys_rename(a, b) == 0) { done++; continue; }
        // The new name went wrong: back to the old one.
        if (k_path_join(dir, olds[i], b, sizeof b)) sys_rename(a, b);
        snprintf(news[i], URENAME_NAME, "%s", olds[i]);
    }
    ulogf("files: renamed %d of %d\n", done, n);
    snprintf(g_stat_note, sizeof g_stat_note, "renamed %d item%s", done, done == 1 ? "" : "s");
    if (done) undo_record_renames(dir, olds, news, n);
    reload_panes();
    refresh_status();
}

void rename_many_open(struct uapp *a) {
    struct uui_fileview *fv = active();
    static char names[UUI_RENAMER_MAX][URENAME_NAME];
    char path[PATH_MAX_LEN];
    int marks = uui_fileview_mark_count(fv), n = 0;
    for (int i = 0; i < marks && n < UUI_RENAMER_MAX; i++)
        if (uui_fileview_marked_path(fv, i, path, sizeof path))
            strlcpy(names[n++], k_path_basename(path), URENAME_NAME);
    if (n < 2) { set_note("mark two or more to rename them together"); return; }
    uui_renamer_open(a, uui_fileview_dir(fv), names, n, rename_many_done, 0);
}
