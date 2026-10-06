#ifndef UUI_FILEDIALOG_H
#define UUI_FILEDIALOG_H

#include <stdint.h>
#include "syscall_abi.h"   // struct sys_dirent, SYS_LISTDIR_MAX
#include "ui/uapp.h"
#include "ui/uui_button.h"
#include "ui/uui_fileview.h"
#include "ui/uui_focus.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_places.h"
#include "ui/uui_pathbar.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_textbox.h"
#include "ui/uui_toolbar.h"

// --- the file chooser, as a window ------------------------------------
//
// ONE CHOOSER FOR EVERY APP, in a window of its own -- Win32's
// GetOpenFileName, Qt's QFileDialog, GtkFileChooserDialog. It is a
// ui/uapp.h uapp_window, not a ui/uui_dialog box drawn inside the app:
// in a small viewer window there is nowhere to put a browsable listing.
// docs/decisions.md ("A file chooser is a window of its own") has why,
// including why the out-of-process portal is deliberately not copied.
//
// **THE FILTER IS THE APP'S, AND THAT IS WHAT MAKES THIS SHAREABLE.**
// Image Viewer decides what an image is by probing magic bytes; a
// chooser that filtered by extension would take that away, and each app
// would go back to writing its own. Same argument uui_fileview.h makes
// one level down.
//
// **PLACES, NOT A TREE.** A folder tree is the File Manager's -- it owns
// the rebuild-and-reveal logic one needs -- and a second copy here would
// be exactly the duplication uui_fileview exists to have ended.

// How many entries one listing holds. SYS_LISTDIR_MAX so nothing is
// silently dropped -- the cost is ~20 KB of BSS in each app that opens
// one, which is the same trade every uui_fileview caller makes.
#define UUI_FILEDIALOG_ENTRIES SYS_LISTDIR_MAX
#define UUI_FILEDIALOG_PATH_MAX UUI_FILEVIEW_PATH_MAX
#define UUI_FILEDIALOG_PLACES 8
#define UUI_FILEDIALOG_FILTERS 8

enum uui_filedialog_mode {
    // "Open": the name must already exist as a file.
    UUI_FILEDIALOG_OPEN,
    // "Save": the name may be new, and is typed as often as chosen.
    UUI_FILEDIALOG_SAVE,
    // "Choose": a DIRECTORY is the answer, and files are listed only so
    // the user can see what is in it. GTK's SELECT_FOLDER action.
    UUI_FILEDIALOG_FOLDER,
};

// One row of the "Files of type" list. Directories are always offered
// whatever `fn` says -- a filter that hid them would make a chooser
// nobody can navigate -- so `fn` only ever decides about FILES, and a
// NULL one lists them all.
struct uui_filedialog_filter {
    const char *label;
    uui_fileview_filter_fn fn;
    void *ctx;
};

// The row every chooser ends with, written once here rather than in
// each app: Win32's "All Files (*.*)", KDE's "All Files".
#define UUI_FILEDIALOG_ALL_FILES { "All files", 0, 0 }

struct uui_filedialog_opts {
    enum uui_filedialog_mode mode;
    const char *title;         // NULL derives one from the mode
    const char *start_dir;     // NULL or "" starts at "/"
    const char *initial_name;  // pre-fills the name field

    // THE "FILES OF TYPE" LIST, caller-owned and outliving the window.
    // Empty means no combo and no filtering -- a chooser for a program
    // that opens anything. Picking a row re-lists.
    const struct uui_filedialog_filter *filters;
    int filter_count;
    int filter_index;          // which row starts selected
};

struct uui_filedialog {
    // Chosen, or NULL on cancel. The window is closed BEFORE this runs,
    // so the callback is free to open another one.
    void (*on_done)(void *ctx, const char *path);
    void *ctx;

    enum uui_filedialog_mode mode;
    struct uapp_window *win;
    char app[48];   // the owner's name, for Recent (lib/urecent.h)

    // --- the content ------------------------------------------------
    struct sys_dirent entries[UUI_FILEDIALOG_ENTRIES];
    struct uui_fileview view;
    struct uui_toolbar bar;
    struct uui_pathbar path;       // the File Manager's breadcrumb, beside the toolbar
    struct uui_places places;      // ...and its Places and Devices
    int swallow_key;               // the breadcrumb took this Enter/Esc, not the dialog
    struct uui_label name_label;
    struct uui_textbox name;
    struct uui_label type_label;
    struct uui_dropdown type;
    const struct uui_filedialog_filter *filters;
    int filter_count;
    // The combo takes LABELS, not our rows, so the pointers are lifted
    // out once at open (ui/uui_dropdown.h). Bounded because the array
    // is the chooser's, not the caller's.
    const char *type_labels[UUI_FILEDIALOG_FILTERS];
    struct uui_label spacer;   // absorbs the button row's leftover width
    struct uui_button ok, cancel;

    struct uui_item top_items[2];
    struct uui_item body_items[2];
    struct uui_item name_items[2];
    struct uui_item type_items[2];
    struct uui_item button_items[3];
    struct uui_item root_items[5];
    struct uui_layout top_row, body, name_row, type_row, button_row, root;

    struct uui_focusable focusables[6];   // see uui_filedialog.c's FOCUS_*
    struct uui_focus focus;

    char title[64];
};

// Opens the chooser as a modal window owned by `a`'s. `fd` is the
// caller's and must outlive the window -- a file-scope struct, as every
// widget here is; it is ~20 KB, so never a local.
//
// Returns the window, or NULL if the compositor refused one. The
// callback runs exactly once, with the chosen path or with NULL.
struct uapp_window *uui_filedialog_open(struct uapp *a, struct uui_filedialog *fd,
                                         const struct uui_filedialog_opts *opts,
                                         void (*on_done)(void *ctx, const char *path),
                                         void *ctx);

// Is one up? An app that must not open a second asks this.
int uui_filedialog_is_open(const struct uui_filedialog *fd);

// Closes it WITHOUT reporting anything -- for an app tearing down while
// a chooser is up. The ordinary ends (a choice, Cancel, the X) all go
// through the callback instead.
void uui_filedialog_close_window(struct uui_filedialog *fd);

#endif // UUI_FILEDIALOG_H
