#ifndef UUI_PREFS_H
#define UUI_PREFS_H

// uui_prefs -- an app's OPTIONS WINDOW: a sidebar of pages, each a column
// of "Caption:  control  [control]" rows, over Defaults / OK / Cancel.
// Dolphin's and Kate's Configure dialogs; Notepad, Screenshot, Terminal
// and the File Manager all open theirs here. Modal; nothing is applied
// until OK; Esc cancels and Return keeps.
//
// **THE WINDOW IS SHARED, THE CONTROLS ARE THE APP'S.** The app owns its
// widgets and their ids (below UUI_PREFS_ID_BASE), fills them from its
// config before uui_prefs_open(), and reads them back in `on_ok`. This
// owns the pages, the rows, the focus ring, the buttons and the window.
//
// **BOUND ROWS ARE THE WINDOW'S TOO.** A row that edits one int field of
// the app's options struct -- a checkbox, a choice, a number -- names the
// field by its offset, and the window fills it from `edit` when it opens,
// resets it through `edit_defaults` on Defaults and writes it back before
// `on_ok`. The struct is the same one lib/uprefs.h loads and saves, so an
// option that is only a key in a file is one table row and one window row,
// and no to_controls/from_controls pair. Rows the window cannot bind -- a
// gallery, a chooser, a key bound in another file -- stay the app's.
//
// ONE AT A TIME per process -- module state, as there is only ever one
// modal Options window up.

#include <stddef.h>

struct uapp;
struct uui_widget_ops;
struct uui_sidebar_row;

#define UUI_PREFS_PAGES_MAX 8
#define UUI_PREFS_ROWS_MAX  32
#define UUI_PREFS_ID_BASE   0x7F00   // the window's own ids; an app's stay below

struct uui_prefs_desc {
    const char *title;
    // A row's `id` is its page's NUMBER, 0 .. page_count - 1.
    const struct uui_sidebar_row *pages;
    int page_count;
    int w, h;                    // content size, a FLOOR: grown to fit the largest page; 0 derives 96 'n' by 30 lines
    const char *log_prefix;      // the layout log's name for it, or NULL
    void (*on_defaults)(void);   // put the defaults INTO THE CONTROLS
    void (*on_ok)(void);         // read the controls back; the window is already gone
    void (*on_widget)(int id, int reason);   // optional: a control changed
    void (*on_action)(int code);             // optional: one of the app's own buttons
    int first_page;              // the page it opens on; 0 is the first
    // The struct the bound rows edit, and how to put its defaults in;
    // Defaults calls this, then `on_defaults` for the app's own rows.
    void *edit;
    void (*edit_defaults)(void *edit);
    // Optional: the window went, however -- OK, Cancel, Esc or its X.
    // For what the app holds only while it is up (a preview's buffers).
    void (*on_close)(void);
};

// Starts a new description, forgetting the last one's rows.
void uui_prefs_begin(const struct uui_prefs_desc *d);

// A row on `page`: a caption and its first control. A gallery gets the
// page's width under its caption rather than beside it. Returns the row,
// or -1 when UUI_PREFS_ROWS_MAX are taken.
int uui_prefs_row(int page, const char *caption, const struct uui_widget_ops *ops,
                  void *widget, int id, const char *name, unsigned flags);

// BOUND ROWS (above). Each returns its row, like uui_prefs_row(), and is
// logged by `name`. `field` is offsetof() an int in `edit`.
//
// A checkbox: the field is 0 or 1. An empty caption stacks it under the
// row above.
int uui_prefs_check(int page, const char *caption, const char *text, size_t field,
                    const char *name);
// Segments: `values[i]` is what segment i stores (NULL: i itself), so a
// row can show "On | Off" over a field where on is 1, or "2 | 4 | 8".
int uui_prefs_choice(int page, const char *caption, const char *const *labels,
                     const int *values, int count, size_t field, const char *name);
// A spinbox over lo..hi, moving by `step`.
int uui_prefs_number(int page, const char *caption, int lo, int hi, int step, const char *unit,
                     size_t field, const char *name);

// A bound checkbox BESIDE `row`'s control -- a cursor and its blink.
void uui_prefs_also_check(int row, const char *text, size_t field, const char *name);

// `row`'s first control at `width` pixels rather than its natural width --
// a slider's track a hand can travel.
void uui_prefs_widen(int row, int width);

// Another control on `row`, after the ones already there (two at most).
void uui_prefs_also(int row, const struct uui_widget_ops *ops, void *widget, int id,
                    const char *name);

// Opens it on `first_page`. 0 if it is already open or could not be.
int uui_prefs_open(struct uapp *a);
int uui_prefs_is_open(void);
void uui_prefs_close(void);
void uui_prefs_redraw(void);

#endif
