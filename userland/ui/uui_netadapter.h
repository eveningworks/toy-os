#ifndef UUI_NETADAPTER_H
#define UUI_NETADAPTER_H

// uui_netadapter -- one network card's ADAPTER SETTINGS as a panel: Link
// speed, Energy Efficient Ethernet, Flow control and Interrupt
// moderation, each a setting row shown only when the card's driver
// offers it, and Restore defaults. Device Manager shows it for the
// selected network card, System Settings on Network > Adapters --
// Windows' Advanced tab and a NetworkManager connection's Ethernet tab,
// in one place each.
//
// **A CHANGE IS APPLIED AND SAVED AT ONCE** (lib/unetlink.h): the card
// does it now, and netd applies it again from /etc/net.conf at boot.
// Like uui_sndformat, a PANEL OF STANDARD CONTROLS: the app embeds
// uui_netadapter_item(), adds the focusables to its ring, offers each
// on_widget id to uui_netadapter_on_widget() and each on_action code to
// uui_netadapter_on_action(), and calls uui_netadapter_fit() after a
// layout (the rows' descriptions wrap).

#include <stdint.h>
#include "ui/uui_widget.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_switch.h"
#include "ui/uui_button.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_setting_row.h"
#include "ui/uui_focus.h"
#include "query_abi.h"

// The ids (and the button's action code) it uses: id_base ..
// id_base + UUI_NETADAPTER_IDS - 1.
#define UUI_NETADAPTER_IDS 8

struct uui_netadapter {
    int id_base;
    int loaded;                       // a card is loaded
    struct query_netdev d;            // the card, as last read back

    struct uui_dropdown speed_dd, flow_dd, mod_dd;
    struct uui_switch eee_sw;
    struct uui_button reset_b;
    struct uui_label note_l;
    struct uui_setting_row speed_r, eee_r, flow_r, mod_r;

    uint32_t speed_cap[7];            // per speed row: the cap, 0 = automatic
    char speed_text[7][32];
    const char *speed_ptr[7];
    int nspeed;
    const char *flow_ptr[4], *mod_ptr[4];
    char eee_desc[96];
    char note[112];

    struct uui_item foot_it[2];
    struct uui_layout foot;
    struct uui_item col_it[6];
    struct uui_layout col;
};

void uui_netadapter_init(struct uui_netadapter *n, int id_base);

// Fills the controls from a card -- QUERY_NETDEV's record. Rows come and
// go with what its driver offers: lay out again after.
void uui_netadapter_load(struct uui_netadapter *n, const struct query_netdev *d);

// The panel, for the app's layout.
struct uui_item uui_netadapter_item(struct uui_netadapter *n);

// Re-derives the rows' wrapping from their current widths; 1 when any
// changed, meaning: lay out again (it settles: width never follows height).
int uui_netadapter_fit(struct uui_netadapter *n);

// Its controls, in tab order, appended to `out`; returns how many.
int uui_netadapter_focusables(struct uui_netadapter *n, struct uui_focusable *out, int max);

// 1 when `id` / `code` was the panel's: the change is applied (or the
// refusal said in the panel's note) -- redraw.
int uui_netadapter_on_widget(struct uui_netadapter *n, int id);
int uui_netadapter_on_action(struct uui_netadapter *n, int code);

#endif
