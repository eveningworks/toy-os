#ifndef UUI_FILEINFO_H
#define UUI_FILEINFO_H

// uui_fileinfo -- a file's preview and facts, drawn from a struct
// ufileinfo (lib/ufileinfo.h): a HERO (its picture on a stage tinted by
// it, else its type's icon; the name and a one-line summary; a folder's
// volume bar) over collapsible SECTIONS of label/value rows. Properties
// shows all of it; the File Manager's details pane the COMPACT form --
// the hero and a few General rows, no section chrome.
//
// TALLER THAN ITS RECT, IT SCROLLS (the wheel), and a section opened
// near the bottom scrolls itself into view.
//
// SLOTS are how an app puts its own controls inside: a section reserves
// a height for them (above its rows), the widget lays it out, and
// uui_fileinfo_slot_rect() says where to place the controls -- 0 while
// that section is closed or scrolled out of view, when they must be hidden.

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uambient.h"
#include "lib/ufileinfo.h"

#define UUI_FI_SECTIONS 8
#define UUI_FI_ROWS     8
#define UUI_FI_VALUE    96

struct uui_fi_section {
    char title[24];
    int role;                       // UTHEME_ACT_*, the caption's colour
    int open;
    int nrows;
    char key[UUI_FI_ROWS][16];
    char val[UUI_FI_ROWS][UUI_FI_VALUE];
    int slot_id, slot_h;            // the app's controls; 0 for none
};

struct uui_fileinfo {
    int x, y, w, h;
    const struct ufileinfo *fi;     // caller-owned
    const struct uimg *preview;     // caller-owned picture, or NULL
    int compact;
    char subtitle[96];
    struct uui_fi_section sec[UUI_FI_SECTIONS];
    int nsec;
    int hot, armed;                 // header under the pointer / pressed; -1 none. OWNED
    int toggled;                    // a section opened or closed; uui_fileinfo_take_toggle()
    int scroll;                     // px scrolled when taller than h (the wheel). OWNED
    // The stage behind a picture, tinted by it. Behind a pointer because
    // draw() takes a const widget and the stage caches its gradient.
    struct uambient amb;
    const struct uimg *amb_for;
    struct uambient_stage *stage;
    struct uambient_stage stage_store;
};

void uui_fileinfo_init(struct uui_fileinfo *w);
// Rebuild the sections from `fi` (and the summary line). A section keeps
// whether it was open, matched by title; slots set before are kept.
void uui_fileinfo_set(struct uui_fileinfo *w, const struct ufileinfo *fi);
// A slot in an existing section (by title), or a new section holding
// only a slot. Returns 0, or -1 when there is no room.
int  uui_fileinfo_slot(struct uui_fileinfo *w, const char *title, int role, int id, int h, int open);
int  uui_fileinfo_slot_rect(const struct uui_fileinfo *w, int id, int *x, int *y, int *ww, int *hh);
// The height everything needs at `width` -- a window sizes itself by it.
int  uui_fileinfo_height(const struct uui_fileinfo *w, int width);
// The thumbnail size (longest edge) that fills the preview stage at
// `width` for this picture's shape -- what to ask uthumb_get() for.
int  uui_fileinfo_preview_px(const struct uui_fileinfo *w, int width);
int  uui_fileinfo_take_toggle(struct uui_fileinfo *w);
// A section header's rect, for a test to click; 0 when there is none.
int  uui_fileinfo_header_rect(const struct uui_fileinfo *w, const char *title, int *x, int *y, int *ww, int *hh);
int  uui_fileinfo_open(struct uui_fileinfo *w, const char *title, int open);

void uui_fileinfo_natural_size(const struct uui_fileinfo *w, int *out_w, int *out_h);
void uui_fileinfo_set_geometry(struct uui_fileinfo *w, int x, int y, int width, int height);
void uui_fileinfo_draw(struct ugfx_surface *s, const struct uui_fileinfo *w);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_fileinfo_ops;

#endif
