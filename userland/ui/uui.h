#ifndef UUI_H
#define UUI_H

// uui -- the ring-3 widget toolkit, and the umbrella include for it.
//
// This file used to BE the toolkit (states, buttons, button groups),
// with everything else in a second grab-bag called uwidgets.h. Both are
// now one file per widget, the same shape apps/ui/ has always had, so a
// widget's kernel-side and ring-3 versions live at matching paths and
// porting between them is a file-to-file comparison rather than a hunt
// through two large files. This header just pulls them all in, exactly
// as apps/ui/ui.h does -- include it and get everything, or include the
// one widget you use.
//
// What survives the split unchanged is the part worth keeping, and it
// is documented per widget:
//
//   * The four interaction states derive from the control's OWN colour,
//     not from a fixed palette (uui_primitives.h). An early version
//     always lightened for hover, which on this near-white theme moved
//     the pixels by two out of 255 -- a hover nobody could see.
//
//   * A press ARMS and a release COMMITS, and a press dragged off its
//     target commits nothing (uui_button_group.h). That is the rule
//     every control in this GUI follows (docs/gui-guidelines.md), and a
//     ring-3 client is not exempt because its events arrive as messages.
//
//   * ONE geometry calculation per widget, shared by draw / hit-test /
//     drag -- the scrollbar is the clearest case. Two copies of that
//     arithmetic drift, and the symptom is a click landing one row off.
//
// The one structural difference from apps/ui/: every draw takes a
// `struct ugfx_surface *` and content-relative coordinates, because a
// client draws into its own buffer and has no screen origin to add.

#include "ui/uui_primitives.h"
#include "ui/uui_button.h"
#include "ui/uui_button_group.h"
#include "ui/uui_scrollbar.h"
#include "ui/uui_textbox.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_radio_list.h"
#include "ui/uui_listbox.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_canvas.h"
#include "ui/uui_focus.h"

#endif
