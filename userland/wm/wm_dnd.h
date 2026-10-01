#ifndef WM_DND_H
#define WM_DND_H
#include <stdint.h>

// Cross-window drag-and-drop, brokered by the compositor -- see wm_dnd.c.
void wm_dnd_start(int pid, int count);       // WIN_REQ_DRAG_START from a client
void wm_dnd_start_desktop(int count);        // a desktop file icon left the desktop
void wm_dnd_end(int pid);                    // WIN_REQ_DRAG_END
int  wm_dnd_active(void);
int  wm_dnd_refused_at(int mx, int my);      // a drop here would be taken by nothing
// Every tick while a button is held, and on the release. After the
// release tick, wm_dnd_took_drop() says whether a window or the
// desktop took the drop -- the desktop's own icon drag reads it to
// leave its icon where it was.
void wm_dnd_motion(int mx, int my, uint8_t buttons);
int  wm_dnd_took_drop(void);
void wm_dnd_draw(int mx, int my);

#endif
