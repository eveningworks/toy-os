#ifndef CRASH_NOTICE_H
#define CRASH_NOTICE_H

#include <stdint.h>

// THE CRASH NOTICE: a card above the tray when a program crashes --
// "Notepad closed unexpectedly", Details and Reopen -- and one after the
// desktop itself restarted. Windows' "display driver stopped responding
// and has recovered" and KDE DrKonqi's notification, in this desktop's
// shape.
//
// **THE KERNEL SAYS WHAT CRASHED** (QUERY_CRASH, this boot's last eight
// ring-3 crashes, each with a `seq`). The WM polls it; it does not infer
// a crash from a window disappearing, which a clean exit also does.
// Details opens /bin/wm/apps/crashreports on the report.
//
// A PASSIVE overlay (wm_overlay.h): it takes clicks on its cards and
// nothing else, and does not count as "a menu is open".

extern int crash_notice_open;

// At startup: remember the newest crash, and if it was the desktop's own
// within the last minute, say the desktop restarted.
void crash_notice_init(void);
// From the frame loop; throttled inside to twice a second.
void crash_notice_poll(void);

void crash_notice_draw(int mx, int my);
int  crash_notice_handle_click(int mx, int my);
int  crash_notice_hover_at(int mx, int my);
void crash_notice_damage(void);
int  crash_notice_rect(int *x, int *y, int *w, int *h);
// For `gui state`: the newest card's title, and its Details and Reopen
// rects as {x, y, w, h} (w 0 when the card has no such button).
const char *crash_notice_describe(int details[4], int reopen[4]);

// A client's WIN_REQ_NOTICE, one piece of its path at a time.
void crash_notice_piece(int pid, int a, int kind, unsigned flags, const char *text);
// The newest card's action `b` (1..4): its label, and its rect in r.
const char *crash_notice_button(int b, int r[4]);
const char *crash_notice_path(void);   // a file card's file, or NULL
const char *crash_notice_sub(void);

#endif
