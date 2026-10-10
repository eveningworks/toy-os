// System Settings: Sound > Output's Test and Format cards -- a speaker
// test (ui/uui_sndtest.h) and the output format of the card in use
// (ui/uui_sndformat.h), under the Output device row.
// Split out as set_clock.c is: a page that needs more than its settings'
// generic rows.
//
// NOT A STAGED SETTING. The format lives in SND_CARDS_FILE, per card,
// not in the settings registry (a fixed catalogue, one row each), so it
// is saved as it is changed -- like Change... on Date & time -- and it
// reaches the card from the next sound that starts.
#include "settings/settings_internal.h"
#include "ui/uui_sndformat.h"
#include "ui/uui_sndtest.h"
#include "query_abi.h"

#define SET_DEVICE "system.audio_device"

int g_snd_page;
static struct uui_sndformat g_fmt;
static struct uui_setting_row g_fmt_row;
static char g_fmt_title[64];
static struct uui_sndtest g_test;
static struct uui_setting_row g_test_row;
static char g_test_title[64];
static int g_fast_tick;
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
    snprintf(g_test_title, sizeof g_test_title, "Test -- %s", q->label);
    g_shown_rate = q->rate;
    g_shown_bits = q->bits;
    strlcpy(g_shown_card, q->name, sizeof g_shown_card);
}

void snd_init(void) {
    uui_sndformat_init(&g_fmt, ID_SNDFMT);
    uui_setting_row_init(&g_fmt_row, g_fmt_title, "", uui_sndformat_item(&g_fmt));
    uui_sndtest_init(&g_test, ID_SNDTEST);
    uui_setting_row_init(&g_test_row, g_test_title, "", uui_sndtest_item(&g_test));
}

void snd_page_opened(void) {
    uui_sndtest_stop(&g_test);   // a test does not outlive its page
    g_snd_page = 0;
    for (int i = 0; i < g_slot_count; i++) if (on_device_slot(i)) g_snd_page = 1;
    struct query_sound q;
    if (!g_snd_page || !active_card(&q)) { g_snd_page = 0; return; }
    load(&q);
}

int snd_emit_after(struct uui_item *out, int n, int i,
                   struct uui_focusable *focus, int *nfocus) {
    if (!g_snd_page || !on_device_slot(i)) return n;
    g_test_row.title = g_test_title;
    g_test_row.desc = "Plays a chime once a second on the side you choose, at the volume above.";
    g_test_row.control = uui_sndtest_item(&g_test);
    g_test_row.stacked = 1;
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_test_row,
                                  .flags = UUI_FILL_W, .name = "sndtest_card" };
    *nfocus += uui_sndtest_focusables(&g_test, focus + *nfocus, UUI_SNDTEST_IDS);
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
    int was = g_test.playing;
    char said[sizeof g_test.status];
    strlcpy(said, g_test.status, sizeof said);
    if (g_snd_page && uui_sndtest_on_widget(&g_test, id)) {
        if (g_test.playing != was || strcmp(said, g_test.status)) {   // not a hover or a press
            ulogf("settings: sndtest %s\n", g_test.status);
            snd_tick(a);
            relayout_page();   // the status line is measured for its text
        }
        return 1;
    }
    if (!g_snd_page || !uui_sndformat_on_widget(&g_fmt, id)) return 0;
    ulogf("settings: sndfmt %s match %d fixed %u allowed %#x bits %u\n", g_fmt.card,
          g_fmt.f.match, (unsigned)g_fmt.f.fixed, (unsigned)g_fmt.f.allowed, (unsigned)g_fmt.f.bits);
    relayout_page();
    return 1;
}

// The Test and Format cards' descriptions, wrapped to the width the
// page gave them, as refit_prose() does for every setting's card.
int snd_fit(void) {
    if (!g_snd_page) return 0;
    int changed = uui_setting_row_fit(&g_test_row);
    if (uui_setting_row_fit(&g_fmt_row)) changed = 1;
    return changed;
}

int snd_on_action(struct uapp *a, int code) {
    if (!uui_sndtest_on_action(&g_test, code)) return 0;
    ulogf("settings: sndtest %s\n", g_test.status);
    snd_tick(a);   // the tick rate follows at once
    relayout_page();
    return 1;
}

void snd_shutdown(void) { uui_sndtest_stop(&g_test); }

// "Playing now" follows the card -- a file starting at another rate
// moves it while the page is open. Asked once a second. A running test
// asks for the tick at 30 Hz for its meters, and gives it back.
int snd_tick(struct uapp *a) {
    int fast = uui_sndtest_playing(&g_test);
    if (fast != g_fast_tick) {
        uapp_set_tick(a, fast ? 33 : SETTINGS_TICK_MS);
        g_fast_tick = fast;
    }
    int moved = uui_sndtest_tick(&g_test);
    if (!g_snd_page) return moved;
    uint64_t now = sys_monotonic_ns() / 1000000ull;
    if (now - g_checked_ms < 1000) return moved;
    g_checked_ms = now;
    struct query_sound q;
    if (!active_card(&q)) return moved;
    if (q.rate == g_shown_rate && q.bits == g_shown_bits && !strcmp(q.name, g_shown_card)) return moved;
    load(&q);
    relayout_page();
    return 1;
}
