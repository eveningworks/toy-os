#ifndef UUI_RENAMER_H
#define UUI_RENAMER_H
// THE RENAME-MANY DIALOG: a window of its own, over lib/urename.h -- a
// rule (Numbered, Find and replace, Change case), Keep extensions, and a
// PREVIEW of every old name beside its new one, with the clashes said
// before anything is touched. Dolphin's "Rename items", Thunar's Bulk
// Rename, PowerRename.
//
// IT RENAMES NOTHING ITSELF: Rename hands the app the pairs that change,
// and the app does the renaming its own way (the File Manager through
// temporary names, so a swap works, and into its undo journal).
#include "ui/uapp.h"
#include "lib/urename.h"

#define UUI_RENAMER_MAX 256   // names in one dialog

// `olds[i]` becomes `news[i]` for each of the `n` that change, all in
// `dir`. Called once, on Rename; the window is closed after.
typedef void (*uui_renamer_done)(void *ctx, const char *dir, char (*olds)[URENAME_NAME],
                                 char (*news)[URENAME_NAME], int n);

// Opens the dialog over `names` (copied) in `dir`; one at a time.
void uui_renamer_open(struct uapp *a, const char *dir, char (*names)[URENAME_NAME], int n,
                      uui_renamer_done done, void *ctx);
int  uui_renamer_is_open(void);
#endif
