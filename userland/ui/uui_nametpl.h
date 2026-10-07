#ifndef UUI_NAMETPL_H
#define UUI_NAMETPL_H
// uui_nametpl -- a FILE-NAME TEMPLATE FIELD: a text field, a row of
// chips that each type a <token> at the caret, and the name the
// template gives today under them ("Next file: shot-20261007-...qoi"),
// or why it gives none. Spectacle's filename template row; the parsing
// is lib/unametpl.h, so the preview and the program that names the file
// cannot disagree.
//
// One widget, one id: the chips are its own parts, not buttons the app
// routes. The router names it to the app on any release, so a chip and
// a click into the field both arrive as `on_widget(id, ...)`.
#include "ui/uui_textbox.h"
#include "lib/unametpl.h"

struct uui_widget_ops;

struct uui_nametpl_chip {
    const char *label;   // "Date"
    const char *token;   // "date" -- typed as "<date>"
};

#define UUI_NAMETPL_CHIPS 8

struct uui_nametpl {
    int x, y, w, h;
    struct uui_textbox field;
    const struct uui_nametpl_chip *chips;
    int chip_count;
    // TODAY'S VALUES, for the preview. The app's, and it may change them
    // between paints (a counter, the time).
    const struct unametpl_var *sample;
    int sample_count;
    const char *suffix;      // added to the preview: ".qoi"; NULL for none
    int pressed;             // the chip held down, or -1
    int in_field;            // the press went to the field: its drag follows
    int disabled;
};

void uui_nametpl_init(struct uui_nametpl *t, const char *initial,
                      const struct uui_nametpl_chip *chips, int chip_count,
                      const struct unametpl_var *sample, int sample_count);
const char *uui_nametpl_text(const struct uui_nametpl *t);
// Is what the field holds a template unametpl_expand() takes?
int uui_nametpl_valid(const struct uui_nametpl *t);

extern const struct uui_widget_ops uui_nametpl_ops;
#endif
