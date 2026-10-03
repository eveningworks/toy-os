#ifndef WM_SHADOW_H
#define WM_SHADOW_H

// DROP SHADOWS UNDER WINDOWS AND POPUPS, drawn by the compositor into
// the scene BEFORE the window that casts them, so the window's rounded
// corners (corners_save/corners_round) reveal shadow rather than
// desktop. Mutter and Plasma both pre-render a blurred rounded rect
// and 9-slice it; nothing blurs per frame here either -- the falloff
// is a table and each corner a cached tile.
//
// THE SHADOW IS PART OF THE WINDOW'S DAMAGE. It paints outside the
// window's rect, so anything that damages a window rect must damage
// the rect PLUS wm_shadow_margin() -- wm_damage_window_rect() -- or a
// moved window leaves its old shadow behind, which `gui damage verify`
// reports as pixels changed outside the box.
//
// The focused window's shadow is larger and darker than an inactive
// one's (Mutter, DWM, macOS): depth says which window is active at a
// glance. Maximized and fullscreen windows cast none -- there is no
// desktop beside them to fall on.

enum wm_shadow_kind {
    WM_SHADOW_NONE = 0,
    WM_SHADOW_INACTIVE,
    WM_SHADOW_FOCUSED,
    WM_SHADOW_POPUP,      // menus, dropdowns, tray flyouts: small
};

// `desktop.shadows`, adopted on the loop's generation poll. A change
// repaints the whole scene once.
void wm_shadow_poll_config(void);
int  wm_shadow_enabled(void);

// How far past its rect ANY shadow can reach, in pixels -- the padding
// a window's damage rect carries. Font-derived, like the radii.
int wm_shadow_margin(void);

// Blend the shadow of a (x, y, w, h) rect whose corners are rounded by
// `corner_r` (0 for square) into the scene. Honours the active clip.
void wm_shadow_draw(int x, int y, int w, int h, int corner_r, enum wm_shadow_kind kind);

// OVERLAPPING SHADOWS COMBINE BY THE DARKEST, NOT THE PRODUCT: a pixel
// already shadowed by alpha `s` since the last opaque paint over it is
// taken only up to a new shadow's `a`, never to 1-(1-s)(1-a). Ten
// identical windows stacked exactly used to compound into a solid black
// ring. So every OPAQUE paint must clear the record under it:
// wm_shadow_draw() clears its own casting rect (the body painted next),
// and anything painted with no shadow first calls wm_shadow_cover().
// Honours the active clip.
void wm_shadow_cover(int x, int y, int w, int h);

// wm_damage_rect() of the rect grown by wm_shadow_margin() on every
// side: what a WINDOW's rect damages.
void wm_damage_window_rect(int x, int y, int w, int h);

#endif // WM_SHADOW_H
