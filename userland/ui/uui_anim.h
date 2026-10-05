#ifndef UUI_ANIM_H
#define UUI_ANIM_H

// FRAMES FOR THINGS THAT MOVE ON THEIR OWN. Toykit draws when an event
// asks it to and then blocks; a widget mid-animation has no event. It
// calls uui_anim_request() from its draw instead, and uapp's pump
// shortens its wait to one frame, repaints, and the widget's next draw
// decides again -- so the process wakes per frame only while something
// is moving, and sleeps as before the rest of the time. One flag for
// the whole process; nothing registers or unregisters, which is what
// lets a widget be destroyed mid-motion without a dangling entry.

#define UUI_ANIM_FRAME_MS 16   // ~60 Hz while something animates

// A widget that is still moving asks for one more frame. Idempotent.
void uui_anim_request(void);

// uapp: was a frame asked for since the last take? Clears the flag.
int uui_anim_take(void);
// uapp: peek without clearing, to size the wait.
int uui_anim_pending(void);

// The clock every toolkit animation paces by (sys_monotonic_ns).
unsigned long long uui_anim_now_ns(void);

// `base_ms` through `desktop.animations` and `desktop.animation_speed`,
// for an APP's own motion: 0 when animations are off or the speed is
// instant -- and 0 means SKIP the effect, never "a very short one" --
// else `base_ms` scaled by the same ratios the window manager uses
// (wm_anim.c's anim_duration_ms(): fast halves, slow doubles, very-slow
// quadruples). Two registry reads, so ask once per effect, not per frame.
unsigned uui_anim_ms(unsigned base_ms);

// --- scrolling policy, in one place -----------------------------------

// `desktop.smooth_scroll`, asked of the registry each time (one syscall
// per wheel notch, not per frame) so a change in System Settings takes
// effect on the next scroll with nothing cached to go stale.
int uui_smooth_scroll_enabled(void);

// How far ONE wheel notch scrolls, for a widget that scrolls by pixel:
// three text lines, Qt's and Explorer's figure. A widget that scrolls
// by row uses three rows, which is the same amount at the row height
// most of them have.
int uui_wheel_step_px(void);

// How long a scroll glides. Qt animates a scrollbar over ~150 ms.
#define UUI_SCROLL_MS 150

#endif // UUI_ANIM_H
