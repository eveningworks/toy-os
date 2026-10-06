// System Settings: Sound > Output's Format card -- the output format of
// the card in use (ui/uui_sndformat.h), under the Output device row.
// Split out as set_clock.c is: a page that needs more than its settings'
// generic rows.
//
// NOT A STAGED SETTING. The format lives in SND_CARDS_FILE, per card,
// not in the settings registry (a fixed catalogue, one row each), so it
// is saved as it is changed -- like Change... on Date & time -- and it
// reaches the card from the next sound that starts.
#include "settings/settings_internal.h"
#include "ui/uui_sndformat.h"
#include "query_abi.h"

#define SET_DEVICE "system.audio_device"

int g_snd_page;
static struct uui_sndformat g_fmt;
static struct uui_setting_row g_fmt_row;
static char g_fmt_title[64];
static uint32_t g_shown_rate, g_shown_bits;
static char g_shown_card[16];
static uint64_t g_checked_ms;

static int on_device_slot(int i) {
    return g_slot[i].setting >= 0 && strcmp(g_name[g_slot[i].setting], SET_DEVICE) == 0;
}

// The card in use, and whether there is one.
static int active_card(struct query_sound *out) {
    struct query_sound q;
    QUERY_FOREACH(QUERY_SOUND, q, qi)
        if (q.active) { *out = q; return 1; }
    return 0;
}

static void load(const struct query_sound *q) {
    uui_sndformat_load(&g_fmt, q);
    snprintf(g_fmt_title, sizeof g_fmt_title, "Format -- %s", q->label);
    g_shown_rate = q->rate;
    g_shown_bits = q->bits;
    strlcpy(g_shown_card, q->name, sizeof g_shown_card);
}

void snd_init(void) {
    uui_sndformat_init(&g_fmt, ID_SNDFMT);
    uui_setting_row_init(&g_fmt_row, g_fmt_title, "", uui_sndformat_item(&g_fmt));
}

void snd_page_opened(void) {
    g_snd_page = 0;
    for (int i = 0; i < g_slot_count; i++) if (on_device_slot(i)) g_snd_page = 1;
    struct query_sound q;
    if (!g_snd_page || !active_card(&q)) { g_snd_page = 0; return; }
    load(&q);
}

int snd_emit_after(struct uui_item *out, int n, int i,
                   struct uui_focusable *focus, int *nfocus) {
    if (!g_snd_page || !on_device_slot(i)) return n;
    g_fmt_row.title = g_fmt_title;
    g_fmt_row.desc = "What the card is sent. Other formats are converted to it. "
                     "A change applies from the next sound.";
    g_fmt_row.control = uui_sndformat_item(&g_fmt);
    g_fmt_row.stacked = 1;
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_fmt_row,
                                  .flags = UUI_FILL_W, .name = "sndfmt_card" };
    *nfocus += uui_sndformat_focusables(&g_fmt, focus + *nfocus, UUI_SNDFORMAT_IDS);
    return n;
}

int snd_on_widget(struct uapp *a, int id) {
    (void)a;
    if (!g_snd_page || !uui_sndformat_on_widget(&g_fmt, id)) return 0;
    ulogf("settings: sndfmt %s match %d fixed %u allowed %#x bits %u\n", g_fmt.card,
          g_fmt.f.match, (unsigned)g_fmt.f.fixed, (unsigned)g_fmt.f.allowed, (unsigned)g_fmt.f.bits);
    relayout_page();
    return 1;
}

// "Playing now" follows the card -- a file starting at another rate
// moves it while the page is open. Asked once a second.
int snd_tick(void) {
    if (!g_snd_page) return 0;
    uint64_t now = sys_monotonic_ns() / 1000000ull;
    if (now - g_checked_ms < 1000) return 0;
    g_checked_ms = now;
    struct query_sound q;
    if (!active_card(&q)) return 0;
    if (q.rate == g_shown_rate && q.bits == g_shown_bits && !strcmp(q.name, g_shown_card)) return 0;
    load(&q);
    relayout_page();
    return 1;
}
