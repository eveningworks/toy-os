// A network card's adapter settings as a panel -- see uui_netadapter.h.
#include <stdio.h>
#include <string.h>
#include "ui/uui_netadapter.h"
#include "ui/utheme.h"
#include "lib/unetlink.h"
#include "rt/sys.h"

enum { ID_SPEED = 0, ID_EEE = 1, ID_FLOW = 2, ID_MOD = 3, ID_RESET = 4 };   // + id_base

void uui_netadapter_init(struct uui_netadapter *n, int id_base) {
    memset(n, 0, sizeof *n);
    n->id_base = id_base;
    uui_dropdown_init(&n->speed_dd, 0, 0, 0, 0, 0, 0);
    uui_dropdown_init(&n->flow_dd, 0, 0, 0, 0, 0, 0);
    uui_dropdown_init(&n->mod_dd, 0, 0, 0, 0, 0, 0);
    uui_switch_init(&n->eee_sw, 0);
    uui_button_init(&n->reset_b, 0, 0, 0, 0, "Restore defaults", UTHEME_BUTTON_BG, UTHEME_TEXT,
                    id_base + ID_RESET);
    uui_label_init(&n->note_l, n->note);
    for (int i = 0; i < 4; i++) {
        n->flow_ptr[i] = unetlink_flow_label((uint32_t)i);
        n->mod_ptr[i] = unetlink_mod_label((uint32_t)i);
    }
    uui_dropdown_set_items(&n->flow_dd, n->flow_ptr, 4);
    uui_dropdown_set_items(&n->mod_dd, n->mod_ptr, 4);
    uui_setting_row_init(&n->speed_r, "Link speed",
                         "The rates the card offers when it negotiates with the switch.",
                         (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &n->speed_dd,
                                            .id = id_base + ID_SPEED, .name = "netadapter_speed" });
    uui_setting_row_init(&n->eee_r, "Energy Efficient Ethernet", n->eee_desc,
                         (struct uui_item){ .ops = &uui_switch_ops, .widget = &n->eee_sw,
                                            .id = id_base + ID_EEE, .name = "netadapter_eee" });
    uui_setting_row_init(&n->flow_r, "Flow control",
                         "Pause frames when a buffer on either side fills.",
                         (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &n->flow_dd,
                                            .id = id_base + ID_FLOW, .name = "netadapter_flow" });
    uui_setting_row_init(&n->mod_r, "Interrupt moderation",
                         "Fewer interrupts for less CPU, at the cost of a little latency.",
                         (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &n->mod_dd,
                                            .id = id_base + ID_MOD, .name = "netadapter_mod" });
}

static void build(struct uui_netadapter *n) {
    uint32_t c = n->loaded ? (uint32_t)n->d.link_caps : 0;
    n->foot_it[0] = (struct uui_item){ .ops = &uui_button_ops, .widget = &n->reset_b,
                                       .id = n->id_base + ID_RESET, .name = "netadapter_reset",
                                       .hidden = !c };
    n->foot_it[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &n->note_l,
                                       .flags = UUI_FILL_W, .name = "netadapter_note" };
    n->foot = (struct uui_layout){ .dir = UUI_ROW, .items = n->foot_it, .count = 2 };
    n->col_it[0] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &n->speed_r,
                                      .flags = UUI_FILL_W, .hidden = !(c & NET_LINK_RATES) };
    n->col_it[1] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &n->eee_r,
                                      .flags = UUI_FILL_W, .hidden = !(c & NET_LINK_EEE) };
    n->col_it[2] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &n->flow_r,
                                      .flags = UUI_FILL_W, .hidden = !(c & NET_LINK_FLOW) };
    n->col_it[3] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &n->mod_r,
                                      .flags = UUI_FILL_W, .hidden = !(c & NET_LINK_MODERATION) };
    n->col_it[4] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &n->foot, .flags = UUI_FILL_W };
    n->col = (struct uui_layout){ .dir = UUI_COLUMN, .items = n->col_it, .count = 5 };
}

static void fill(struct uui_netadapter *n) {
    const struct query_netdev *d = &n->d;
    uint32_t sup = (uint32_t)d->rates_supported;

    // Speed: Automatic, then each slower cap the card has, fastest first.
    uint32_t fastest = 0;
    for (uint32_t b = 1; b <= NET_RATE_ALL; b <<= 1) if (sup & b) fastest = b;
    int k = 0, sel = 0;
    n->speed_cap[k] = 0;
    snprintf(n->speed_text[k], sizeof n->speed_text[k], "Automatic (up to %s)",
             unetlink_rate_label(fastest));
    k++;
    uint32_t now = unetlink_cap_of(sup, (uint32_t)d->rates);
    for (uint32_t b = fastest >> 1; b && k < 7; b >>= 1) {
        if (!(sup & b)) continue;
        n->speed_cap[k] = b;
        snprintf(n->speed_text[k], sizeof n->speed_text[k], "Up to %s", unetlink_rate_label(b));
        if (now == b) sel = k;
        k++;
    }
    n->nspeed = k;
    for (int i = 0; i < k; i++) n->speed_ptr[i] = n->speed_text[i];
    uui_dropdown_set_items(&n->speed_dd, n->speed_ptr, k);
    uui_dropdown_set_selected(&n->speed_dd, sel);

    n->eee_sw.on = d->eee ? 1 : 0;
    snprintf(n->eee_desc, sizeof n->eee_desc, "Saves power between frames. Some switches lose "
             "traffic with it on.%s", d->eee_active ? " In use on this link." : "");
    n->eee_r.desc = n->eee_desc;
    uui_dropdown_set_selected(&n->flow_dd, (int)(d->flow & 3));
    uui_dropdown_set_selected(&n->mod_dd, d->moderation <= NET_MOD_HIGH ? (int)d->moderation : (int)NET_MOD_MEDIUM);
}

void uui_netadapter_load(struct uui_netadapter *n, const struct query_netdev *d) {
    n->d = *d;
    n->loaded = 1;
    if (d->link_caps) {
        fill(n);
        snprintf(n->note, sizeof n->note, "Applied at once, and kept for this card.");
    } else {
        snprintf(n->note, sizeof n->note, "This card's driver (%s) offers no adapter settings.", d->driver);
    }
    uui_label_set_text(&n->note_l, n->note);
    build(n);
}

struct uui_item uui_netadapter_item(struct uui_netadapter *n) {
    if (!n->col.items) build(n);
    return (struct uui_item){ .ops = &uui_layout_ops, .widget = &n->col, .flags = UUI_FILL_W };
}

int uui_netadapter_fit(struct uui_netadapter *n) {
    int changed = 0;
    struct uui_setting_row *rows[4] = { &n->speed_r, &n->eee_r, &n->flow_r, &n->mod_r };
    for (int i = 0; i < 4; i++) if (!n->col_it[i].hidden && uui_setting_row_fit(rows[i])) changed = 1;
    return changed;
}

int uui_netadapter_focusables(struct uui_netadapter *n, struct uui_focusable *out, int max) {
    int k = 0;
    if (!n->loaded || !n->d.link_caps) return 0;
    if (!n->col_it[0].hidden && k < max) out[k++] = (struct uui_focusable){ &n->speed_dd, &uui_dropdown_ops };
    if (!n->col_it[1].hidden && k < max) out[k++] = (struct uui_focusable){ &n->eee_sw, &uui_switch_ops };
    if (!n->col_it[2].hidden && k < max) out[k++] = (struct uui_focusable){ &n->flow_dd, &uui_dropdown_ops };
    if (!n->col_it[3].hidden && k < max) out[k++] = (struct uui_focusable){ &n->mod_dd, &uui_dropdown_ops };
    if (k < max) out[k++] = (struct uui_focusable){ &n->reset_b, &uui_button_ops };
    return k;
}

// The card read back after a change: what the driver actually did, by
// MAC, since netd may rename it meanwhile.
static void reread(struct uui_netadapter *n) {
    struct query_netdev q;
    QUERY_FOREACH(QUERY_NETDEV, q, qi)
        if (q.mac == n->d.mac) { n->d = q; fill(n); return; }
}

static void said(struct uui_netadapter *n, int r) {
    if (r < 0) snprintf(n->note, sizeof n->note, "Not applied: %s.", sys_strerror(sys_errno()));
    else snprintf(n->note, sizeof n->note, "Applied, and kept for this card.");
    uui_label_set_text(&n->note_l, n->note);
}

int uui_netadapter_on_widget(struct uui_netadapter *n, int id) {
    int k = id - n->id_base;
    if (k < 0 || k >= UUI_NETADAPTER_IDS || !n->loaded || !n->d.link_caps) return 0;
    struct net_linkcfg want;
    memset(&want, 0, sizeof want);
    if (k == ID_SPEED) {
        int sel = uui_dropdown_selected(&n->speed_dd);
        if (sel < 0 || sel >= n->nspeed) return 1;
        want.which = NET_LINK_RATES;
        want.rates = unetlink_rates_for((uint32_t)n->d.rates_supported, n->speed_cap[sel]);
    } else if (k == ID_EEE) {
        want.which = NET_LINK_EEE;
        want.eee = n->eee_sw.on ? 1 : 0;
    } else if (k == ID_FLOW) {
        want.which = NET_LINK_FLOW;
        want.flow = (uint32_t)uui_dropdown_selected(&n->flow_dd) & 3;
    } else if (k == ID_MOD) {
        int sel = uui_dropdown_selected(&n->mod_dd);
        want.which = NET_LINK_MODERATION;
        want.moderation = sel >= 0 && sel <= (int)NET_MOD_HIGH ? (uint32_t)sel : NET_MOD_MEDIUM;
    } else {
        return 0;
    }
    said(n, unetlink_set(&n->d, &want));
    reread(n);
    return 1;
}

int uui_netadapter_on_action(struct uui_netadapter *n, int code) {
    if (code != n->id_base + ID_RESET || !n->loaded || !n->d.link_caps) return 0;
    int r = unetlink_reset(&n->d);
    reread(n);
    if (r < 0) said(n, r);
    else {
        snprintf(n->note, sizeof n->note, "Back to the driver's defaults.");
        uui_label_set_text(&n->note_l, n->note);
    }
    return 1;
}
