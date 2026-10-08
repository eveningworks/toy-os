// System Settings: Network > Adapters -- each network card's adapter
// settings (ui/uui_netadapter.h), the card chosen at the top, with what
// it is connected as. A page of its own, as set_startup.c's Boot menu
// is: the settings live per card in /etc/net.conf (lib/unetlink.h), not
// in the settings registry, and are applied as they change, like Sound's
// Format -- nothing waits for Apply.
#include "settings/settings_internal.h"
#include "ui/uui_netadapter.h"
#include "lib/unetlink.h"

#define MAX_CARDS 4   // NET_MAX_DEVS

int g_show_adapters;
static struct uui_netadapter g_na;
static struct query_netdev g_cards[MAX_CARDS];
static int g_ncards, g_cur;
static char g_card_text[MAX_CARDS][48];
static const char *g_card_ptr[MAX_CARDS];
static struct uui_dropdown g_card_dd;
static struct uui_button g_dm_b;
static struct uui_setting_row g_card_row, g_conn_row;
static struct uui_label g_none_l;
static char g_conn[160];
static uint64_t g_checked_ms;

void adapters_init(void) {
    uui_netadapter_init(&g_na, ID_NETADP);
    uui_dropdown_init(&g_card_dd, 0, 0, 0, 0, 0, 0);
    uui_button_init(&g_dm_b, 0, 0, 0, 0, "Open in Device Manager", UTHEME_BUTTON_BG, UTHEME_TEXT,
                    ID_NA_DEVMGR);
    uui_setting_row_init(&g_card_row, "Adapter", "The network card these settings are for.",
                         (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_card_dd,
                                            .id = ID_NA_CARD, .name = "na_card" });
    uui_setting_row_init(&g_conn_row, "Connection", g_conn,
                         (struct uui_item){ .ops = &uui_button_ops, .widget = &g_dm_b,
                                            .id = ID_NA_DEVMGR, .name = "na_devmgr" });
    uui_label_init(&g_none_l, "There are no network cards.");
}

static void ip_text(char *out, size_t cap, uint64_t ip) {
    snprintf(out, cap, "%u.%u.%u.%u", (unsigned)(ip >> 24) & 255, (unsigned)(ip >> 16) & 255,
             (unsigned)(ip >> 8) & 255, (unsigned)ip & 255);
}

// "Connected, 2.5 Gb/s. 192.168.200.161." -- what the flyout says, here
// so a change to the speed is seen where it is made.
static void connection_text(const struct query_netdev *d) {
    char speed[24] = "", ip[24];
    uint64_t b = d->link_bps;
    if (b >= 1000000000ull && b % 1000000000ull)
        snprintf(speed, sizeof speed, ", %u.%u Gb/s", (unsigned)(b / 1000000000ull),
                 (unsigned)(b % 1000000000ull / 100000000ull));
    else if (b >= 1000000000ull) snprintf(speed, sizeof speed, ", %u Gb/s", (unsigned)(b / 1000000000ull));
    else if (b) snprintf(speed, sizeof speed, ", %u Mb/s", (unsigned)(b / 1000000ull));
    const char *state = d->admin_down ? "Switched off" : !d->link_known ? "Link unknown"
                      : d->link_up ? "Connected" : "No cable or no link";
    if (d->ip) {
        ip_text(ip, sizeof ip, d->ip);
        snprintf(g_conn, sizeof g_conn, "%s%s. Address %s.", state, d->link_up ? speed : "", ip);
    } else {
        snprintf(g_conn, sizeof g_conn, "%s%s. No address.", state, d->link_up ? speed : "");
    }
    g_conn_row.desc = g_conn;
}

void adapters_load(void) {
    uint64_t keep = g_ncards ? g_cards[g_cur].mac : 0;
    g_ncards = 0;
    struct query_netdev q;
    QUERY_FOREACH(QUERY_NETDEV, q, qi) {
        if (g_ncards >= MAX_CARDS) break;
        g_cards[g_ncards] = q;
        snprintf(g_card_text[g_ncards], sizeof g_card_text[0], "%s (%s%s%s)", q.name, q.driver,
                 q.location[0] ? ", " : "", q.location);
        g_card_ptr[g_ncards] = g_card_text[g_ncards];
        g_ncards++;
    }
    g_cur = 0;
    for (int i = 0; i < g_ncards; i++) if (g_cards[i].mac == keep) g_cur = i;
    uui_dropdown_set_items(&g_card_dd, g_card_ptr, g_ncards);
    uui_dropdown_set_selected(&g_card_dd, g_cur);
    if (g_ncards) {
        uui_netadapter_load(&g_na, &g_cards[g_cur]);
        connection_text(&g_cards[g_cur]);
    }
    ulogf("settings: adapters %d card %s caps %#llx\n", g_ncards, g_ncards ? g_cards[g_cur].name : "-",
          g_ncards ? (unsigned long long)g_cards[g_cur].link_caps : 0ull);
}

int adapters_emit(struct uui_item *out, int n, struct uui_focusable *focus, int *nfocus) {
    if (!g_ncards) {
        out[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_none_l, .flags = UUI_FILL_W };
        return n;
    }
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_card_row,
                                  .flags = UUI_FILL_W, .name = "na_card_row" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_card_dd, &uui_dropdown_ops };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_conn_row,
                                  .flags = UUI_FILL_W, .name = "na_conn_row" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_dm_b, &uui_button_ops };
    struct uui_item panel = uui_netadapter_item(&g_na);
    panel.name = "netadapter";
    out[n++] = panel;
    *nfocus += uui_netadapter_focusables(&g_na, focus + *nfocus, UUI_NETADAPTER_IDS);
    return n;
}

int adapters_fit(void) {
    int changed = 0;
    if (!g_ncards) return 0;
    if (uui_setting_row_fit(&g_card_row)) changed = 1;
    if (uui_setting_row_fit(&g_conn_row)) changed = 1;
    if (uui_netadapter_fit(&g_na)) changed = 1;
    return changed;
}

int adapters_on_widget(int id) {
    if (!g_show_adapters) return 0;
    if (id == ID_NA_CARD) {
        int sel = uui_dropdown_selected(&g_card_dd);
        if (sel >= 0 && sel < g_ncards && sel != g_cur) {
            g_cur = sel;
            uui_netadapter_load(&g_na, &g_cards[g_cur]);
            connection_text(&g_cards[g_cur]);
        }
        return 1;
    }
    if (!uui_netadapter_on_widget(&g_na, id)) return 0;
    ulogf("settings: netadapter %s rates %#llx eee %llu flow %llu moderation %llu: %s\n", g_na.d.name,
          (unsigned long long)g_na.d.rates, (unsigned long long)g_na.d.eee,
          (unsigned long long)g_na.d.flow, (unsigned long long)g_na.d.moderation, g_na.note);
    return 1;
}

int adapters_on_action(int code) {
    if (!g_show_adapters) return 0;
    if (code == ID_NA_DEVMGR) {
        sys_spawn("/bin/wm/system/devmgr", 0, -1);
        return 1;
    }
    if (!uui_netadapter_on_action(&g_na, code)) return 0;
    ulogf("settings: netadapter %s reset: %s\n", g_na.d.name, g_na.note);
    return 1;
}

// The connection line follows the card: a link coming up after a speed
// change is what the person is waiting to see. Asked once a second.
int adapters_tick(void) {
    if (!g_show_adapters || !g_ncards) return 0;
    uint64_t now = sys_monotonic_ns() / 1000000ull;
    if (now - g_checked_ms < 1000) return 0;
    g_checked_ms = now;
    struct query_netdev q;
    QUERY_FOREACH(QUERY_NETDEV, q, qi) {
        if (q.mac != g_cards[g_cur].mac) continue;
        char was[sizeof g_conn];
        strlcpy(was, g_conn, sizeof was);
        g_cards[g_cur] = q;
        connection_text(&q);
        return strcmp(was, g_conn) != 0;
    }
    return 0;
}
