#ifndef LEAVE_PAGE_H
#define LEAVE_PAGE_H

// The Leave page: Restart, Shut down and Exit to shell as one full-screen
// page over the desktop, dimmed and blurred -- KDE Plasma's Leave screen
// (mockup P2, 2026-10-04). Start's three power rows open it with theirs
// focused.
//
// **IT ASKS EVERY APP TO CLOSE FIRST**, as Windows, KDE and GNOME do: each
// client window gets the polite close, quietly (wm_request_close_quiet(),
// via close_batch.h), the page waits,
// and an app that is still open after a few seconds -- Notepad asking
// whether to save -- is listed, with Cancel (back to the desktop, where
// its question is waiting) or "<action> anyway" (mockup R2). There is no
// countdown: nothing here acts unless a button is pressed.
#include <stdint.h>

enum leave_action { LEAVE_RESTART, LEAVE_SHUTDOWN, LEAVE_EXIT, LEAVE_ACTIONS };

extern int leave_page_open;

void leave_page_show(enum leave_action focus);
void leave_page_cancel(void);
// Each frame: which asked apps have gone, and acting once they all have.
void leave_page_poll(void);

// The overlay registry's ops (wm_overlay.h).
void leave_page_draw(int mx, int my);
int  leave_page_handle_click(int mx, int my);
void leave_page_update_press(int mx, int my, uint8_t buttons);
int  leave_page_hover_at(int mx, int my);
void leave_page_damage(void);
int  leave_page_key(int key, uint8_t mods);

// For `gui leave`: the phase, the focus, every control's rect by name,
// and the apps asked to close. `dry` set makes the final action a log
// line instead -- a test cannot power the machine off and go on asking.
const char *leave_page_phase(void);
int leave_page_focus(void);
int leave_page_control(int i, const char **name, int *x, int *y, int *w, int *h);
int leave_page_app(int i, const char **title, int *gone);
void leave_page_set_dry_run(int on);

// The page's snapshot hides the whole scene: the renderer need not draw
// the windows, the desktop or the taskbar under it.
int leave_page_covers(void);

#endif
