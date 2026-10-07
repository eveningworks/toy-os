#ifndef ULIB_UNAMETPL_H
#define ULIB_UNAMETPL_H

// A FILE-NAME TEMPLATE: literal text with <tokens> the caller fills --
// "shot-<date>-<time>" -> "shot-20261007-142233". Spectacle's filename
// template, GNOME's `auto-save-directory` cousin; Windows has none.
//
// THE CALLER NAMES THE TOKENS, so one parser serves any program that
// names files: a screenshot, an exported report. A token nobody named,
// a '<' with no '>', or a '/' anywhere is REFUSED rather than written
// through -- a template that half-works names files wrongly for ever.
//
// AN EMPTY VALUE TAKES ONE SEPARATOR WITH IT ("-", "_", " ", "."), so
// "shot-<date>-<window>" with no window is "shot-20261007", not
// "shot-20261007-". Values are the caller's and are written as given:
// pass anything user-visible (a window title) through unametpl_clean().

struct unametpl_var {
    const char *name;    // without the brackets: "date"
    const char *value;   // what it becomes; "" or NULL for nothing
};

// `tpl` expanded into `out`. 1, or 0 -- writing nothing usable -- for a
// token not in `vars`, an unclosed '<', a '/', or a result that is empty
// or does not fit.
int unametpl_expand(const char *tpl, const struct unametpl_var *vars, int n,
                    char *out, int cap);

// Does `tpl` name a token not in `vars`, or fail as unametpl_expand()
// would on any values? 1 when it is good.
int unametpl_valid(const char *tpl, const struct unametpl_var *vars, int n);

// `in` made safe as part of a file name: letters, digits, '.', '_' and
// '-' kept, every other run of characters one '-', none at either end,
// at most `cap - 1` bytes. "notes.md -- Notepad" -> "notes.md-Notepad".
void unametpl_clean(const char *in, char *out, int cap);

#endif
