#ifndef UUI_CARD_H
#define UUI_CARD_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_widget.h"
#include "ui/uui_layout.h"

// A CARD THAT HOLDS A GROUP: a title and a line under it, an optional
// control at the right of that header -- a switch that turns the whole
// group on -- and a column of items below. WinUI's SettingsExpander
// header with its content showing, libadwaita's PreferencesGroup with a
// header suffix, Qt's QGroupBox with a checkable title.
//
// A CONTAINER: the header control and every body item are children, so
// each reports its own id and takes focus as it would anywhere. Body
// items that are uui_setting_rows should be `flat`, or they draw a card
// inside the card.
//
// `active` outlines the card in the accent -- "this group is on" -- and
// `badge` puts a pill after the title ("1 connected"). Both are the
// caller's to keep current.
struct uui_card {
    int x, y, w, h;
    const char *title;           // caller-owned; bold
    const char *subtitle;        // caller-owned; may be NULL
    const char *badge;           // caller-owned; NULL for none
    int active;
    struct uui_item kids[2];     // OWNED: [0] the header control, [1] the body
    struct uui_layout body;      // a column over the caller's items
};

// `header` may be a zeroed item (no control). `items` stays the caller's.
void uui_card_init(struct uui_card *c, const char *title, const char *subtitle,
                   struct uui_item header, struct uui_item *items, int count);

extern const struct uui_widget_ops uui_card_ops;

#endif
