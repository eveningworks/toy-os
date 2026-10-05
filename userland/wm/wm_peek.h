#ifndef WM_PEEK_H
#define WM_PEEK_H

// TASKBAR PEEK: resting the pointer on a window button shows a card
// above it with a scaled image of the window -- Windows 11's taskbar
// preview, Plasma's task tooltip. A grouped button shows one entry per
// window. Clicking an entry activates that window and closes the card;
// its x asks the window to close and leaves the card up, so a group can
// be closed one x after another. `desktop.taskbar_peek`:
//
//   off        the plain title tooltip, as before
//   preview    the card (the DEFAULT)
//   highlight  the card, and while the pointer is on an entry the rest
//              of the desktop dims under that window -- Plasma's
//              "Highlight windows", Windows' Aero Peek
//
// An OVERLAY in wm_overlay.c's table, drawn straight into the
// compositor's buffer like the tooltip it replaces. Entries are held by
// `struct window.open_seq`, never by windows[] index: a click or a
// raise reorders windows[] under an open card.

#include <stdint.h>

enum wm_peek_mode { WM_PEEK_OFF, WM_PEEK_PREVIEW, WM_PEEK_HIGHLIGHT };
void wm_peek_set_mode(enum wm_peek_mode m);
enum wm_peek_mode wm_peek_mode(void);

// Can this window be shown in a card? A ring-3 client that has
// presented at least one frame. The taskbar keeps the plain tooltip for
// anything else.
int wm_peek_can_show(int idx);

// Called by taskbar_update_hover() once per frame. `wins`/`n` are the
// windows[] indices the hovered button stands for (n == 0: the pointer
// is on no window button); bx/bw the button's x and width.
void wm_peek_hover(const int *wins, int n, int bx, int bw, int mx, int my);

// A client presented a frame: its thumbnail is stale (wm_client.c).
void wm_peek_presented(uint32_t open_seq);

// The window the highlight mode lifts above the dim, or -1.
int wm_peek_highlight_index(void);

// The overlay's ops (wm_overlay.h).
extern int wm_peek_open;
void wm_peek_draw(int mx, int my);
int  wm_peek_click(int mx, int my);
int  wm_peek_hover_at(int mx, int my);
void wm_peek_damage(void);
int  wm_peek_rect(int *x, int *y, int *w, int *h);
void wm_peek_close(void);

// For `gui peek --json`: the card and each entry's thumbnail and close
// rects, in the order drawn. Returns the entry count; 0 when closed.
struct wm_peek_entry_info {
    const char *title;
    int thumb_x, thumb_y, thumb_w, thumb_h;
    int close_x, close_y, close_s;
};
int wm_peek_state(int *x, int *y, int *w, int *h,
                  struct wm_peek_entry_info *out, int max);

#endif
