#ifndef UTHEME_H
#define UTHEME_H

#include "ui/ugfx.h"

// The ring-3 mirror of apps/theme.h's named colours.
//
// Only the ones a ported client actually uses so far -- the same
// "add a colour when a second real caller needs it" bar apps/theme.h
// holds itself to (see its top comment and CLAUDE.md). Copying the
// whole palette across would be inventing an API for nobody.
//
// KEEP THESE IN STEP WITH apps/theme.h. They are duplicated rather than
// shared because a colour constant is three numbers, while sharing the
// header would drag gfx.h (and therefore the whole kernel API surface)
// into every client. That trade is only defensible while the list is
// this short: if it grows past a handful, share the file instead of
// growing the copy. A client whose colours drift from the desktop's is
// exactly the failure the font mapping was designed to avoid, so this
// is the one place that risk is knowingly accepted.

#define UTHEME_WHITE      ugfx_rgb(255, 255, 255) // display/text backgrounds
#define UTHEME_TEXT       ugfx_rgb(20, 20, 20)    // near-black body text
#define UTHEME_BUTTON_BG  ugfx_rgb(225, 225, 230) // light grey button face
#define UTHEME_PANEL_BG   ugfx_rgb(245, 245, 245) // window/panel background

#endif
