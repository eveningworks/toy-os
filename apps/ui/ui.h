#ifndef UI_H
#define UI_H

// Umbrella header for toy-os's small retained-widget-object library --
// a GUI app includes this ONE file instead of hunting down one #include
// per widget it uses. Every ui_*.h in this directory gets added here
// too, so an app that already has `#include "ui/ui.h"` picks up new
// widgets automatically the next time one's added, with no per-app
// change needed.
//
// This is purely a convenience aggregate -- it adds no declarations of
// its own. See ui_button.h's top comment for the actual design
// philosophy shared by everything under apps/ui/ (Brutal-OS-inspired
// owned-state objects, deliberately scaled down for toy-os's
// immediate-mode GUI -- no view tree, no layout DSL).
#include "ui_button.h"
#include "ui_button_group.h"
#include "ui_textbox.h"

#endif
