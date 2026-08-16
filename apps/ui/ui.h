#ifndef UI_H
#define UI_H

// Umbrella header for toy-os's small retained-widget-object library --
// a GUI app includes this ONE file instead of hunting down one #include
// per widget it uses. Every ui_*.h in this directory gets added here
// too, so an app that already has `#include "ui/ui.h"` picks up new
// widgets automatically the next time one's added, with no per-app
// change needed.
//
// As of the full apps/widgets.c/.h -> apps/ui/ migration (see
// docs/decisions.md), this is now genuinely every GUI widget toy-os
// has: ui_primitives (widget_hit/widget_button, the base primitives),
// ui_scrollback (the scrolling text-buffer widget), ui_scrollbar (its
// companion scrollbar), ui_checkbox, ui_button/ui_button_group,
// ui_textbox, and ui_icon_grid (icon-grid geometry + drag-to-reposition,
// shared by the desktop icon grid and any future icon view). apps/widgets.c/.h no longer exist.
//
// This is purely a convenience aggregate -- it adds no declarations of
// its own. See ui_button.h's top comment for the actual design
// philosophy shared by everything under apps/ui/ (Brutal-OS-inspired
// owned-state objects, deliberately scaled down for toy-os's
// immediate-mode GUI -- no view tree, no layout DSL). ui_scrollback/
// ui_scrollbar are the one exception to "owns its geometry": they stay
// plain stateful structs/functions the way they always were, since
// every real caller recomputes their content area live from the
// window's current size on every frame anyway.
// Four widgets left this file in Milestone 41's stage 0
// (docs/wm-ring3-design.md): the checkbox, dropdown, listbox and text
// view had no caller once the GUI apps moved to ring 3, and their
// ring-3 twins under userland/ui/ are the ones that survive the
// milestone. What is left is what the WINDOW MANAGER itself still uses
// -- plus ui_focus, which has no user of its own but is what
// ui_button_group and ui_textbox export their focus tables INTO; it
// retires with them when the WM moves in stage 4.
#include "ui_primitives.h"
#include "ui_scrollback.h"
#include "ui_scrollbar.h"
#include "ui_button.h"
#include "ui_button_group.h"
#include "ui_textbox.h"
#include "ui_icon_grid.h"
#include "ui_radio_list.h"
#include "ui_focus.h"

#endif
