// The name prompt: Rename, New folder and New text file when Options
// says "in a dialog" -- the card dialog (ui/uui_dialog.h) with a text
// field as its body. The in-place rename is the fileview's own
// (uui_fileview_begin_rename) and does not come here.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
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
