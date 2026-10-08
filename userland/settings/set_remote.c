// System Settings: Network > Remote Desktop -- one card per protocol
// (VNC, RDP), each its own switch, port and sign-in; who may connect,
// shared below them; the trusted addresses; the sessions open now. A page
// of its own, as Adapters is: the settings live in /etc/remote.conf
// (lib/uremote.h), read by /bin/remoted every few seconds, and a change
// is written at once -- nothing waits for Apply.
#include "settings/settings_internal.h"
#include "ui/uui_card.h"
#include "ui/uui_actionlist.h"
#include "lib/uremote.h"

int g_show_remote;

static struct uremote_conf g_rc;

// The VNC card.
static struct uui_switch g_vnc_sw;
static struct uui_spinbox g_vnc_port;
static struct uui_textbox g_vnc_pw;
static struct uui_button g_vnc_pw_save;
static struct uui_item g_pw_items[2];
static struct uui_layout g_pw_row;
static struct uui_setting_row g_vnc_port_row, g_vnc_pw_row;
static const char *const ENC[] = { "When the viewer offers it", "Required", "Off" };
static struct uui_dropdown g_enc_dd;
static struct uui_setting_row g_enc_row;
static char g_enc_desc[200];
static struct uui_label g_vnc_note;
static struct uui_item g_vnc_body[4];
static struct uui_card g_vnc;
static char g_vnc_sub[96], g_vnc_badge[24];

// The RDP card: there, so the shape is the one chosen, and honest about
// what is not built.
static struct uui_switch g_rdp_sw;
static struct uui_label g_rdp_note;
static struct uui_item g_rdp_body[1];
static struct uui_card g_rdp;

static struct uui_item g_cards_items[2];
static struct uui_layout g_cards;

// Shared.
static const char *const WHEN[] = { "Ask every time", "Ask unless trusted", "Always allow" };
static const char *const FROM[] = { "This network only", "Anywhere" };
static struct uui_dropdown g_when_dd, g_from_dd;
static struct uui_setting_row g_when_row, g_from_row;

// Trusted viewers.
static struct uui_label g_trusted_h;
static struct uui_actionlist_row g_trusted_rows[UREMOTE_TRUSTED_MAX];
static struct uui_actionlist g_trusted;
static struct uui_textbox g_add_addr, g_add_label;
static struct uui_button g_add_b;
static struct uui_item g_add_items[3];
static struct uui_layout g_add_row_l;
static struct uui_setting_row g_add_row;

// Connected.
static struct uui_label g_conn_h;
#define SESS_MAX 8
static struct query_remotesess g_sess[SESS_MAX];
static int g_nsess;
static struct uui_actionlist_row g_sess_rows[SESS_MAX];
static struct uui_actionlist g_conn;
static uint64_t g_checked_ms;

static const char *addr_text(void) {
    static char out[64];
    struct query_netdev d;
    out[0] = 0;
    QUERY_FOREACH(QUERY_NETDEV, d, i) {
        if (!d.ip) continue;
        uremote_fmt_ip((uint32_t)d.ip, out, sizeof out);
        break;
    }
    return out;
}

static void build_trusted(void) {
    for (int i = 0; i < g_rc.ntrusted; i++) {
        struct uui_actionlist_row *r = &g_trusted_rows[i];
        memset(r, 0, sizeof *r);
        if (g_rc.trusted[i].label[0])
            snprintf(r->title, sizeof r->title, "%s - %s", g_rc.trusted[i].text, g_rc.trusted[i].label);
        else
            snprintf(r->title, sizeof r->title, "%s", g_rc.trusted[i].text);
        int bits = 0;
        for (uint32_t m = g_rc.trusted[i].mask; m; m <<= 1) bits++;
        snprintf(r->sub, sizeof r->sub, bits == 32 ? "Allowed without asking"
                 : "Every address in this subnet is allowed without asking");
        r->btn[1] = "Remove";
        r->primary = -1;
    }
    g_trusted.count = g_rc.ntrusted;
}

// Rebuilds the Connected rows; 1 if what they say changed.
static int build_sessions(void) {
    static char was[SESS_MAX * 160];
    char now[SESS_MAX * 160];
    now[0] = 0;
    g_nsess = uremote_sessions(g_sess, SESS_MAX);
    for (int i = 0; i < g_nsess; i++) {
        struct uui_actionlist_row *r = &g_sess_rows[i];
        memset(r, 0, sizeof *r);
        char ip[16], what[QUERY_REMOTESESS_STATUS_MAX];
        uremote_fmt_ip((uint32_t)g_sess[i].remote_ip, ip, sizeof ip);
        uremote_describe(&g_sess[i], what, sizeof what);
        snprintf(r->title, sizeof r->title, "%s", ip);
        snprintf(r->sub, sizeof r->sub, "%s", what);
        if (uremote_is_desktop(&g_sess[i]))
            r->btn[0] = strstr(what, "view only") ? "Full control" : "View only";
        r->btn[1] = "Disconnect";
        r->primary = -1;
        strlcat(now, r->title, sizeof now);
        strlcat(now, r->sub, sizeof now);
    }
    g_conn.count = g_nsess;
    int changed = strcmp(now, was) != 0;
    strlcpy(was, now, sizeof was);
    return changed;
}

static void describe_cards(void) {
    // The address a viewer types, which is the one fact the card owes;
    // which viewers work is in the note.
    const char *a = addr_text();
    if (a[0]) snprintf(g_vnc_sub, sizeof g_vnc_sub, "%s:%d", a, g_rc.vnc.port);
    else snprintf(g_vnc_sub, sizeof g_vnc_sub, "No address yet");
    int desk = 0;
    for (int i = 0; i < g_nsess; i++) desk += uremote_is_desktop(&g_sess[i]);
    if (desk) snprintf(g_vnc_badge, sizeof g_vnc_badge, "%d connected", desk);
    g_vnc.badge = desk ? g_vnc_badge : NULL;
    g_vnc.active = g_rc.vnc.enabled;
}

void remote_init(void) {
    uui_switch_init(&g_vnc_sw, 0);
    uui_spinbox_init(&g_vnc_port, 5900, 1, 65535, 1, NULL);
    uui_textbox_init(&g_vnc_pw, "");
    g_vnc_pw.masked = 1;
    g_vnc_pw.placeholder = "Not set";
    uui_button_init(&g_vnc_pw_save, 0, 0, 0, 0, "Save", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_RD_PW_SAVE);
    g_pw_items[0] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_vnc_pw,
                                       .id = ID_RD_PW, .flags = UUI_FILL_W, .name = "rd_vnc_pw" };
    g_pw_items[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_vnc_pw_save,
                                       .id = ID_RD_PW_SAVE, .name = "rd_vnc_pw_save" };
    g_pw_row = (struct uui_layout){ .dir = UUI_ROW, .items = g_pw_items, .count = 2 };
    uui_setting_row_init(&g_vnc_port_row, "Port", "5900 is VNC's own.",
                         (struct uui_item){ .ops = &uui_spinbox_ops, .widget = &g_vnc_port,
                                            .id = ID_RD_PORT, .name = "rd_vnc_port" });
    uui_setting_row_init(&g_vnc_pw_row, "Password", "Required: with none, every viewer is refused.",
                         (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_pw_row,
                                            .flags = UUI_FILL_W, .name = "rd_vnc_pw_row" });
    g_vnc_port_row.flat = g_vnc_pw_row.flat = 1;
    g_vnc_pw_row.stacked = 1;
    uui_dropdown_init(&g_enc_dd, 0, 0, 0, 0, ENC, 3);
    uui_setting_row_init(&g_enc_row, "Encryption", g_enc_desc,
                         (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_enc_dd,
                                            .id = ID_RD_ENC, .name = "rd_enc" });
    g_enc_row.flat = 1;
    g_enc_row.stacked = 1;
    uui_label_init(&g_vnc_note, "For TigerVNC, RealVNC, Remmina and macOS Screen Sharing. "
                                "Without encryption VNC checks only the first 8 characters of "
                                "the password, and the picture crosses the network as it is.");
    uui_label_set_wrap(&g_vnc_note, 4);
    g_vnc_body[0] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_vnc_port_row,
                                       .flags = UUI_FILL_W, .name = "rd_vnc_port_row" };
    g_vnc_body[1] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_vnc_pw_row,
                                       .flags = UUI_FILL_W, .name = "rd_vnc_pw_row" };
    g_vnc_body[2] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_enc_row,
                                       .flags = UUI_FILL_W, .name = "rd_enc_row" };
    g_vnc_body[3] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_vnc_note,
                                       .flags = UUI_FILL_W, .name = "rd_vnc_note" };
    uui_card_init(&g_vnc, "VNC", g_vnc_sub,
                  (struct uui_item){ .ops = &uui_switch_ops, .widget = &g_vnc_sw, .id = ID_RD_VNC,
                                     .name = "rd_vnc" },
                  g_vnc_body, 4);

    uui_switch_init(&g_rdp_sw, 0);
    g_rdp_sw.disabled = 1;
    uui_label_init(&g_rdp_note, "Not built yet. RDP is planned after encryption for VNC.");
    uui_label_set_wrap(&g_rdp_note, 3);
    g_rdp_body[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_rdp_note,
                                       .flags = UUI_FILL_W, .name = "rd_rdp_note" };
    uui_card_init(&g_rdp, "RDP", "Windows Remote Desktop",
                  (struct uui_item){ .ops = &uui_switch_ops, .widget = &g_rdp_sw, .id = ID_RD_RDP,
                                     .name = "rd_rdp" },
                  g_rdp_body, 1);
    g_cards_items[0] = (struct uui_item){ .ops = &uui_card_ops, .widget = &g_vnc,
                                          .flags = UUI_FILL_W, .name = "rd_vnc_card" };
    g_cards_items[1] = (struct uui_item){ .ops = &uui_card_ops, .widget = &g_rdp,
                                          .flags = UUI_FILL_W, .name = "rd_rdp_card" };
    g_cards = (struct uui_layout){ .dir = UUI_ROW, .items = g_cards_items, .count = 2 };

    uui_dropdown_init(&g_when_dd, 0, 0, 0, 0, WHEN, 3);
    uui_dropdown_init(&g_from_dd, 0, 0, 0, 0, FROM, 2);
    uui_setting_row_init(&g_when_row, "When someone connects",
                         "The password is checked first, whichever you pick.",
                         (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_when_dd,
                                            .id = ID_RD_WHEN, .name = "rd_when" });
    uui_setting_row_init(&g_from_row, "Allow connections from",
                         "This network: only addresses on a network this machine's cards are on.",
                         (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_from_dd,
                                            .id = ID_RD_FROM, .name = "rd_from" });

    uui_label_init(&g_trusted_h, "Trusted viewers");
    uui_actionlist_init(&g_trusted, g_trusted_rows, 0,
                        "No trusted addresses: everyone is asked about, unless "
                        "\"When someone connects\" says otherwise.");
    uui_textbox_init(&g_add_addr, "");
    g_add_addr.placeholder = "Address or subnet, 192.168.1.0/24";
    uui_textbox_init(&g_add_label, "");
    g_add_label.placeholder = "Name (optional)";
    uui_button_init(&g_add_b, 0, 0, 0, 0, "Add", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_RD_ADD);
    g_add_items[0] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_add_addr,
                                        .id = ID_RD_ADD_ADDR, .flags = UUI_FILL_W, .name = "rd_add_addr" };
    g_add_items[1] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_add_label,
                                        .id = ID_RD_ADD_LABEL, .flags = UUI_FILL_W, .name = "rd_add_label" };
    g_add_items[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_add_b,
                                        .id = ID_RD_ADD, .name = "rd_add" };
    g_add_row_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_add_items, .count = 3 };
    uui_setting_row_init(&g_add_row, "Add a trusted address", NULL,
                         (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_add_row_l,
                                            .flags = UUI_FILL_W, .name = "rd_add_row" });
    g_add_row.stacked = 1;

    uui_label_init(&g_conn_h, "Connected");
    uui_actionlist_init(&g_conn, g_sess_rows, 0, "Nobody is connected.");
}

// Bold headings: a font, not a flag, and set once the session's font is
// known (settings.c calls this where it sets its own title's).
void remote_fonts(void) {
    g_trusted_h.font = ugfx_font_session(UGFX_FONT_BOLD);
    g_conn_h.font = ugfx_font_session(UGFX_FONT_BOLD);
}

void remote_load(void) {
    uremote_load(&g_rc);
    g_vnc_sw.on = g_rc.vnc.enabled;
    uui_spinbox_set_value(&g_vnc_port, g_rc.vnc.port);
    uui_textbox_set_text(&g_vnc_pw, g_rc.vnc.password);
    uui_dropdown_set_selected(&g_when_dd, (int)g_rc.when);
    uui_dropdown_set_selected(&g_enc_dd, (int)g_rc.vnc.encryption);
    // The fingerprint a viewer shows when it first meets this machine:
    // comparing the two is how a person knows nobody is in between.
    if (g_rc.vnc.fingerprint[0])
        snprintf(g_enc_desc, sizeof g_enc_desc, "TLS (VeNCrypt). Certificate SHA-256: %s",
                 g_rc.vnc.fingerprint);
    else
        snprintf(g_enc_desc, sizeof g_enc_desc, "TLS (VeNCrypt). The certificate is made when "
                 "VNC is first switched on.");
    uui_dropdown_set_selected(&g_from_dd, (int)g_rc.from);
    build_trusted();
    build_sessions();
    describe_cards();
}

int remote_emit(struct uui_item *out, int n, struct uui_focusable *focus, int *nfocus) {
    out[n++] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_cards,
                                  .flags = UUI_FILL_W, .name = "rd_cards" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_vnc_sw, &uui_switch_ops };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_vnc_port, &uui_spinbox_ops };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_vnc_pw, &uui_textbox_focus_ops };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_vnc_pw_save, &uui_button_ops };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_enc_dd, &uui_dropdown_ops };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_when_row,
                                  .flags = UUI_FILL_W, .name = "rd_when_row" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_when_dd, &uui_dropdown_ops };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_from_row,
                                  .flags = UUI_FILL_W, .name = "rd_from_row" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_from_dd, &uui_dropdown_ops };
    out[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_trusted_h,
                                  .flags = UUI_FILL_W, .name = "rd_trusted_h" };
    out[n++] = (struct uui_item){ .ops = &uui_actionlist_ops, .widget = &g_trusted, .id = ID_RD_TRUSTED,
                                  .flags = UUI_FILL_W | UUI_TRACK_HOVER, .name = "rd_trusted" };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_add_row,
                                  .flags = UUI_FILL_W, .name = "rd_add_row" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_add_addr, &uui_textbox_focus_ops };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_add_label, &uui_textbox_focus_ops };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_add_b, &uui_button_ops };
    out[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_conn_h,
                                  .flags = UUI_FILL_W, .name = "rd_conn_h" };
    out[n++] = (struct uui_item){ .ops = &uui_actionlist_ops, .widget = &g_conn, .id = ID_RD_CONN,
                                  .flags = UUI_FILL_W | UUI_TRACK_HOVER, .name = "rd_conn" };
    return n;
}

int remote_fit(void) {
    int changed = 0;
    if (uui_setting_row_fit(&g_vnc_port_row)) changed = 1;
    if (uui_setting_row_fit(&g_vnc_pw_row)) changed = 1;
    if (uui_setting_row_fit(&g_enc_row)) changed = 1;
    if (uui_setting_row_fit(&g_when_row)) changed = 1;
    if (uui_setting_row_fit(&g_from_row)) changed = 1;
    if (uui_setting_row_fit(&g_add_row)) changed = 1;
    return changed;
}

static void status(const char *what, int rc) {
    snprintf(g_status, sizeof g_status, rc ? "Could not save %s to /etc/remote.conf." : "%s saved.",
             what);
    ulogf("settings: remote %s %s\n", what, rc ? "FAILED" : "saved");
}

int remote_on_widget(int id) {
    if (!g_show_remote) return 0;
    switch (id) {
    case ID_RD_VNC:
        status(g_vnc_sw.on ? "VNC on" : "VNC off", uremote_set_enabled("vnc", g_vnc_sw.on));
        break;
    case ID_RD_PORT:
        status("The port", uremote_set_port("vnc", uui_spinbox_value(&g_vnc_port)));
        break;
    case ID_RD_ENC: {
        int sel = uui_dropdown_selected(&g_enc_dd);
        if (sel >= 0 && sel < 3) status("Encryption", uremote_set_encryption("vnc", (enum uremote_enc)sel));
        break;
    }
    case ID_RD_WHEN: {
        int sel = uui_dropdown_selected(&g_when_dd);
        if (sel >= 0 && sel < 3) status("When someone connects", uremote_set_when((enum uremote_when)sel));
        break;
    }
    case ID_RD_FROM: {
        int sel = uui_dropdown_selected(&g_from_dd);
        if (sel >= 0 && sel < 2) status("Allow connections from", uremote_set_from((enum uremote_from)sel));
        break;
    }
    case ID_RD_TRUSTED: {
        int r = g_trusted.fired_row;
        g_trusted.fired_row = -1;
        if (r < 0 || r >= g_rc.ntrusted) return 1;
        status("The trusted list", uremote_untrust(g_rc.trusted[r].text));
        break;
    }
    case ID_RD_CONN: {
        int r = g_conn.fired_row, b = g_conn.fired_btn;
        g_conn.fired_row = -1;
        if (r < 0 || r >= g_nsess) return 1;
        int pid = (int)g_sess[r].pid;
        if (b == 1) uremote_disconnect(pid);
        else uremote_set_view_only(pid, !strstr(g_sess_rows[r].sub, "view only"));
        ulogf("settings: remote session %d %s\n", pid, b == 1 ? "disconnect" : "view only toggled");
        return 1;
    }
    default:
        return 0;
    }
    remote_load();
    return 1;
}

int remote_on_action(int code) {
    if (!g_show_remote) return 0;
    if (code == ID_RD_PW_SAVE) {
        status("The password", uremote_set_password("vnc", uui_textbox_text(&g_vnc_pw)));
        remote_load();
        return 1;
    }
    if (code == ID_RD_ADD) {
        struct uremote_net n;
        const char *a = uui_textbox_text(&g_add_addr);
        if (!uremote_parse_net(a, &n)) {
            snprintf(g_status, sizeof g_status, "\"%s\" is not an address -- try 192.168.1.20 "
                     "or 192.168.1.0/24.", a);
            return 1;
        }
        status("The trusted list", uremote_trust(a, uui_textbox_text(&g_add_label)));
        uui_textbox_set_text(&g_add_addr, "");
        uui_textbox_set_text(&g_add_label, "");
        remote_load();
        return 1;
    }
    return 0;
}

// Who is connected follows the machine: asked once a second.
int remote_tick(void) {
    if (!g_show_remote) return 0;
    uint64_t now = sys_monotonic_ns() / 1000000ull;
    if (now - g_checked_ms < 1000) return 0;
    g_checked_ms = now;
    int changed = build_sessions();
    if (changed) describe_cards();
    return changed;
}
