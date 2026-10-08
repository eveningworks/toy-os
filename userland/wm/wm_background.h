#ifndef WM_BACKGROUND_H
#define WM_BACKGROUND_H

#include <stdint.h>

// THE BACKGROUND CLIENT: a live wallpaper, or an animated picture, drawn
// by a process of its own beneath everything -- Wayland's background
// layer (wlr-layer-shell), what swaybg and mpvpaper are clients of.
//
// **IT IS NOT A WINDOW.** windows[] is the stack people work with, and
// forty-odd places walk it for the taskbar, focus, Alt+Tab, hit testing,
// covering and animation; a background row would have to opt out of
// every one, and the first that forgot would put the wallpaper on the
// taskbar. So its surface lives here, in one slot of its own, and the
// desktop draws it where it would draw the picture (desktop.c) -- under
// the icons, which is why it cannot simply be the bottom window either.
//
// ONLY THE PROCESS THIS COMPOSITOR SPAWNED gets the role. Any other
// program from /bin/wm/wallpapers -- a person at a shell, a test --
// opens an ordinary window, which is a preview, not a takeover.
//
// IT DRAWS WHEN THE DESKTOP CAN BE SEEN. Its frames are paced by the
// compositor's timer, and the timer is withheld while a window fills
// the screen or a screensaver runs (`desktop.wallpaper_pause`), so a
// hidden background costs nothing -- the shape of Wayland's frame
// callbacks, which a compositor does not send to a hidden surface.

// Once per frame, after desktop_poll_config(): starts, stops or replaces
// the client to match the settings, and restarts one that died.
void wm_bg_poll(void);

// The hooks wm_client.c calls before its own handling. Each returns 1
// when the message was the background client's and has been handled.
int wm_bg_create(int pid, uint32_t id, int w, int h, int *accepted);
int wm_bg_present(int pid, uint32_t id, int front, uint32_t gen, int w, int h, uint32_t seq);
int wm_bg_destroy(int pid, uint32_t id);
int wm_bg_timer(int pid, uint32_t id, unsigned ms);
void wm_bg_client_gone(int pid);
void wm_bg_check_timer(uint64_t now_ns);
uint64_t wm_bg_timer_due(void);   // 0 when none is armed or it is paused
void wm_bg_screen_changed(void);

// The frame to draw, when there is one at the screen's size. NULL means
// draw the picture or the plain colour instead -- before the first
// frame, after a crash, or with no live background chosen.
const uint32_t *wm_bg_frame(void);

// For `gui background`: its state as one JSON object.
int wm_bg_describe(char *out, int cap);

#endif // WM_BACKGROUND_H
