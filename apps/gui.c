#include "gui.h"
#include "wm/wm.h"

void gui_main(void) {
    // Still the RING-0 desktop, and the flip is one line away
    // (`gui3_main()`), deliberately not taken yet.
    //
    // The ring-3 desktop RUNS -- see `gui3`, apps/gui3.c -- and draws,
    // loads its cursor theme, reads its desktop entries and answers the
    // debug console. What it cannot do yet is give a CLIENT a window:
    // a spawned app creates none and logs nothing, so every tool that
    // opens one fails. Flipping `gui` before that is fixed would turn
    // all 23 tools red at once and leave nothing to diagnose with.
    //
    // Measured with the flip in place, so this is a rate rather than a
    // guess: `desktop_entries` 12/14 and `cursor_theme` 5/9 pass
    // against the ring-3 desktop; everything that needs a client window
    // fails. See docs/roadmap.md.
    wm_run();
}
