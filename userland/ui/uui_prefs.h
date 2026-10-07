#ifndef UUI_PREFS_H
#define UUI_PREFS_H

// uui_prefs -- an app's OPTIONS WINDOW: a sidebar of pages, each a column
// of "Caption:  control  [control]" rows, over Defaults / OK / Cancel.
// Dolphin's and Kate's Configure dialogs, and the shape Terminal and File
// Manager Options were each built in by hand. Modal; nothing is applied
// until OK; Esc cancels and Return keeps.
//
// **THE WINDOW IS SHARED, THE CONTROLS ARE THE APP'S.** The app owns its
// widgets and their ids (below UUI_PREFS_ID_BASE), fills them from its
// config before uui_prefs_open(), and reads them back in `on_ok`. This
// owns the pages, the rows, the focus ring, the buttons and the window.
//
// ONE AT A TIME per process -- module state, as there is only ever one
// modal Options window up.

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
    int w, h;                    // content size; 0 derives 96 'n' by 30 lines
    const char *log_prefix;      // the layout log's name for it, or NULL
    void (*on_defaults)(void);   // put the defaults INTO THE CONTROLS
    void (*on_ok)(void);         // read the controls back; the window is already gone
    void (*on_widget)(int id, int reason);   // optional: a control changed
    void (*on_action)(int code);             // optional: one of the app's own buttons
    int first_page;              // the page it opens on; 0 is the first
};

// Starts a new description, forgetting the last one's rows.
void uui_prefs_begin(const struct uui_prefs_desc *d);

// A row on `page`: a caption and its first control. A gallery gets the
// page's width under its caption rather than beside it. Returns the row,
// or -1 when UUI_PREFS_ROWS_MAX are taken.
int uui_prefs_row(int page, const char *caption, const struct uui_widget_ops *ops,
                  void *widget, int id, const char *name, unsigned flags);

// Another control on `row`, after the ones already there (two at most).
void uui_prefs_also(int row, const struct uui_widget_ops *ops, void *widget, int id,
                    const char *name);

// Opens it on `first_page`. 0 if it is already open or could not be.
int uui_prefs_open(struct uapp *a);
int uui_prefs_is_open(void);
void uui_prefs_close(void);
void uui_prefs_redraw(void);

#endif
