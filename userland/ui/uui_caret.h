#ifndef UUI_CARET_H
#define UUI_CARET_H

// THE TEXT CARET'S BLINK -- one phase for the whole process, asked by
// every widget that draws a caret (uui_textbox, utext, so uui_textview).
// Windows (GetCaretBlinkTime, 530 ms), Qt (cursorFlashTime, 1000 ms a
// cycle) and GTK blink one caret per app the same way. GTK's
// gtk-cursor-blink-timeout is copied too: TEN SECONDS AFTER THE LAST
// INPUT THE CARET STOPS, SOLID, so an idle window sits still
// (docs/gui-guidelines.md, "An idle screen must SIT STILL").
//
// Input restarts the cycle ON, so the caret is never missing under a key
// just typed. `desktop.caret_blink` off draws it solid always. uapp wakes
// for the next flip only while a caret was drawn in the current phase;
// nothing registers, so a widget destroyed mid-blink leaves nothing
// dangling (the uui_anim.h shape).

#define UUI_CARET_HALF_MS    500    // on for this long, then off as long
#define UUI_CARET_TIMEOUT_MS 10000  // then solid until the next input

// Draw a caret now? ALSO records that one was drawn, which is what makes
// uapp wake for the next flip -- so ask it only where a caret is drawn.
int uui_caret_visible(void);

// uapp: input arrived. Solid now, blink restarts; re-reads the setting
// at most every two seconds (this runs on every key, caret or not).
void uui_caret_reset(void);

// uapp: milliseconds until a drawn caret next changes, or -1 when none
// is blinking. 0 = repaint now, returned ONCE per phase -- so call it
// once per wait, and act on the 0.
int uui_caret_wait_ms(void);

#endif
