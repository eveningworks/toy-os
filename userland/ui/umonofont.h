#ifndef UMONOFONT_H
#define UMONOFONT_H

// umonofont -- the desktop's MONOSPACE face at a size an app chooses, or
// the session's own when it chooses none: what a terminal grid or a
// text document draws in. One rasterized atlas per holder, rebuilt only
// when the size changes.
//
// **THE MONOSPACE FACE (`system.font_mono`), never `system.font_face`.**
// A configured size must not undo the family split: the windows that
// most need a fixed cell would be the ones drawing in the interface's.
// A face that will not rasterize falls back to the session's font and
// says so in the log, rather than leaving an empty window.

#include "ui/ugfx.h"

struct umonofont {
    struct ugfx_font font;
    void *arena;
    int px;     // what `font` holds; 0 = following the desktop
    int asked;  // the size last asked for: a failed one is not retried per frame
};

// The font to draw in at `px` (0: the session's monospace face), loading
// it on a change. `who` prefixes the log line a failed load writes.
const struct ugfx_font *umonofont_get(struct umonofont *m, int px, const char *who);

#endif
