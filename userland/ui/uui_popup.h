#ifndef UUI_POPUP_H
#define UUI_POPUP_H

#include "ui/ugfx.h"

// A POPUP SURFACE, as a widget sees it.
//
// A menu, a dropdown list or a tooltip wants to be drawn OUTSIDE the
// window that owns it -- on Windows it is a #32768 HWND, under Wayland an
// xdg_popup -- and toy-os's compositor now offers the same
// (abi/win_proto.h's WIN_REQ_POPUP). But a widget knows nothing about
// windows, channels or buffers: that is uapp's. So the two meet here, at
// a PROVIDER the app model installs once and every widget asks through.
// GTK's GdkPopup is this seam; a widget that can also draw its popup
// inside the window (every one here can) treats a refusal as "do that".
//
// COORDINATES ARE THE PARENT WINDOW'S CONTENT COORDINATES throughout --
// the anchor a widget passes in and the position that comes back -- so
// a widget keeps its popup rect in the space it already hit-tests in.
// Only its DRAWING moves: the surface `uui_popup_surface()` hands back
// is the popup's own, with the popup's top-left at (0, 0), so a widget
// draws at (rect.x - x, rect.y - y). Input arriving on the popup is
// translated back into that same space before any widget sees it, which
// is why nothing but the draw call changes when a popup leaves the
// window.
//
// `done` is called when the COMPOSITOR dismissed the popup -- a press
// outside every surface of this process. The id is already dead; the
// widget closes whatever state it kept and repaints. A popup opened
// WITHOUT UUI_POPUP_GRAB is never dismissed this way, so its `done` is
// only ever the courtesy path: that widget closes its own popup.

#define UUI_POPUP_BELOW 0 // below the anchor, left edges aligned (a menu title)
#define UUI_POPUP_RIGHT 1 // right of the anchor, top edges aligned (a submenu)

// THE GRAB IS WHAT SEPARATES A MENU FROM A TOOLTIP, and it is opt-in --
// Wayland's split, where the surface comes from `xdg_surface.get_popup`
// and `xdg_popup.grab` is a separate request. A grabbing popup owns the
// pointer and a press outside dismisses it; one without routes input as
// if it were not there. A menu WANTS it; a tooltip that took it would
// eat the click meant for the button under the pointer.
#define UUI_POPUP_GRAB  0x01u

struct uui_popup_ops {
    // 0 on refusal; else an id > 0 and where it landed in *out_x/*out_y.
    int (*open)(void *ctx, int ax, int ay, int aw, int ah, int w, int h,
                int gravity, unsigned flags, void (*done)(void *owner),
                void *owner, int *out_x, int *out_y);
    void (*close)(void *ctx, int id);
    // The buffer to draw THIS FRAME into, sized w x h; marks it for
    // presenting. NULL if the popup is gone.
    struct ugfx_surface *(*surface)(void *ctx, int id);
};

void uui_popup_set_provider(const struct uui_popup_ops *ops, void *ctx);

// The widget-facing calls; every one is a refusal/no-op with no provider.
int uui_popup_open(int ax, int ay, int aw, int ah, int w, int h, int gravity,
                   unsigned flags, void (*done)(void *owner), void *owner,
                   int *out_x, int *out_y);
void uui_popup_close(int id);
struct ugfx_surface *uui_popup_surface(int id);

// --- the popup's SHAPE, one answer for every popup ----------------------
//
// Windows 11's and Plasma's menus, dropdown lists and tooltips share one
// rounded, hairline-edged card. The compositor rounds a popup SURFACE's
// corners itself (wm_render.c, as it does a window's) at this radius, so
// a widget drawing into a surface paints a SQUARE card and lets the
// compositor cut it; one drawing in-window (no provider -- the WM's own
// menus) rounds with uui_fill_round_rect(). Font-derived: 8 px at 14 px.
int uui_popup_radius(void);
uint32_t uui_popup_bg(void);      // a step lighter than the chrome
uint32_t uui_popup_border(void);  // the card's hairline

#endif
