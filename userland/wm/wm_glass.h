#ifndef WM_GLASS_H
#define WM_GLASS_H

#include <stdint.h>

// TRANSPARENCY: the taskbar, Start, menus and (by choice) windows drawn
// as GLASS over what is beneath them -- Windows 11's Acrylic and Mica,
// KWin's Blur and Translucency effects. Settings: Appearance >
// Transparency (`desktop.transparency*`); off by default.
//
// THREE KINDS, chosen per surface: CLEAR blends the tint over the scene
// as it is; FROSTED blurs the scene under the rect first (live, cached by
// what is under it); WALLPAPER shows the WALLPAPER blurred once and
// cached, whatever windows are under it (Mica). A see-through WINDOW
// takes its surface's kind over what it saved from beneath itself.
//
// A GLASS SURFACE CASTS A HOLLOW SHADOW (wm_shadow_draw_hollow()): an
// opaque body hid the shade painted under its own edges, glass shows it.
//
// FROSTED GLASS IS PART OF ITS RECT'S DAMAGE: the blur of a pixel reads
// its neighbours, so a damage box touching a frosted rect must cover all
// of it -- wm_render.c grows the box over wm_glass_frosted_rects() before
// the frame is drawn. CLEAR and WALLPAPER need nothing: each pixel
// depends on the pixel under it alone.

struct window;

enum wm_glass_surface { WM_GLASS_TASKBAR, WM_GLASS_START, WM_GLASS_MENU, WM_GLASS_WINDOW };

// `desktop.transparency*`, adopted on the loop's generation poll. A change
// repaints the whole scene once. Also installs the menu painter
// (ui/uui_popup.h's uui_popup_set_glass()), and damages a window whose
// drag starts or ends while "while moving" is on.
void wm_glass_poll_config(void);

// Is that surface glass right now? (On, and its opacity below 100 %.)
int wm_glass_on(enum wm_glass_surface surf);

// Paints (x, y, w, h), rounded by `r`, as that surface's glass tinted
// `tint` -- or fills it with `tint` when the surface is not glass.
void wm_glass_paint(enum wm_glass_surface surf, int x, int y, int w, int h, int r,
                    uint32_t tint);
// The same with a BAND inside it -- a second tint laid over part of the
// glass (Start's lighter app column), or filled solid when it is not
// glass. Part of what a frosted rect caches, so pass it here rather
// than painting it after.
struct wm_glass_band { int x, y, w, h; uint32_t c; uint8_t a; };
void wm_glass_paint_band(enum wm_glass_surface surf, int x, int y, int w, int h, int r,
                         uint32_t tint, const struct wm_glass_band *band);

// The `bg` to hand a text call on that surface: UGFX_TRANSPARENT when
// the text sits straight on its glass ground (`bg == ground`), so glyph
// edges blend against the glass rather than against a flat colour.
uint32_t wm_glass_ink_bg(enum wm_glass_surface surf, uint32_t bg, uint32_t ground);

// A WIN_POPUP_GLASS client's content, in place of wm_client_draw().
void wm_glass_draw_client(const struct window *w);

// Glass title bars (`desktop.transparency_windows=titlebars`).
int wm_glass_titlebars(void);

// How opaque window `i` is drawn, 255 when solid: an inactive or every
// window, or the one being dragged, by the settings. Never a popup or a
// fullscreen window.
uint8_t wm_glass_window_alpha(int i, int focus);
// Around ONE window's drawing: save what is beneath it, then lay that
// back over the window at 255 - alpha. Nested calls are not supported.
void wm_glass_window_begin(int x, int y, int w, int h);
void wm_glass_window_end(int x, int y, int w, int h, uint8_t alpha);

// Every rect drawn as FROSTED glass this frame, for the damage growth
// (see above). Returns how many it wrote.
struct wm_glass_rect { int x, y, w, h; };
int wm_glass_frosted_rects(struct wm_glass_rect *out, int max);

#endif // WM_GLASS_H
