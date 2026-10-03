// System Settings: the System Information page -- KDE's "About this
// System" shape: the system's mark and version, then a Software card and
// a Hardware card of label/value rows, with Copy and Device Manager under
// them. One custom-drawn item in the page's scroll view, so it scrolls
// like any other page; the two buttons are real widgets.
#include "settings/settings_internal.h"
#include "lib/unum.h"
#include "lib/icon_cache.h"
#include "lib/udevice.h"
#include "lib/uclip.h"
#include "build_date.h"
#include "query_abi.h"

#define SI_ROWS 16

struct si_row { const char *key; char val[128]; };

static struct si_row g_sw[SI_ROWS], g_hw[SI_ROWS];
static int g_sw_n, g_hw_n;
static char g_title[64];
static int g_loaded_once;

struct uui_custom g_si_view;
struct uui_button g_si_copy, g_si_devmgr;
// "Kernel and debugging settings": shows the `Debug=1` pages in the
// sidebar (set_registry.c). A view preference, so it acts at once rather
// than staging for Apply.
struct uui_checkbox g_si_debug_cb;
struct uui_setting_row g_si_debug;

static void add(struct si_row *rows, int *n, const char *key, const char *val) {
    if (*n >= SI_ROWS || !val[0]) return;          // a fact nobody reported is not a row
    rows[*n].key = key;
    strlcpy(rows[*n].val, val, sizeof rows[*n].val);
    (*n)++;
}

static void human(uint64_t bytes, char *out, int cap) {
    const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    int i = 0;
    uint64_t whole = bytes, tenth = 0;
    while (whole >= 1024 && i < 4) { tenth = (whole % 1024) * 10 / 1024; whole /= 1024; i++; }
    if (i && whole < 100) snprintf(out, cap, "%llu.%llu %s", (unsigned long long)whole,
                                   (unsigned long long)tenth, u[i]);
    else snprintf(out, cap, "%llu %s", (unsigned long long)whole, u[i]);
    unum_localize(out, (unsigned long)cap, 0);
}

// The rows that change while the page is open: memory in use and uptime.
static void live_rows(void) {
    char v[128], a[32], b[32];
    struct sys_info si;
    memset(&si, 0, sizeof si);
    sys_sysinfo(&si);
    for (int i = 0; i < g_hw_n; i++)
        if (!strcmp(g_hw[i].key, "Memory")) {
            human(si.mem_total_kb * 1024, a, sizeof a);
            human((si.mem_total_kb - si.mem_free_kb) * 1024, b, sizeof b);
            snprintf(g_hw[i].val, sizeof g_hw[i].val, "%s (%s in use)", a, b);
        }
    uint64_t min = sys_monotonic_ns() / 60000000000ull;
    if (min < 1) snprintf(v, sizeof v, "less than a minute");
    else if (min < 120) snprintf(v, sizeof v, "%llu minute%s", (unsigned long long)min, min == 1 ? "" : "s");
    else snprintf(v, sizeof v, "%llu hours %llu minutes", (unsigned long long)(min / 60),
                  (unsigned long long)(min % 60));
    for (int i = 0; i < g_sw_n; i++)
        if (!strcmp(g_sw[i].key, "Up for")) strlcpy(g_sw[i].val, v, sizeof g_sw[i].val);
}

// Everything else, once per visit: udevice_list() reads pci.ids.
void sysinfo_load(void) {
    char v[128], a[32], b[32];
    g_sw_n = g_hw_n = 0;
    snprintf(g_title, sizeof g_title, "toy-os %s", TOYOS_VERSION);

    struct query_version kv;
    if (sys_query_record(QUERY_VERSION, 0, &kv, sizeof kv) >= (int)sizeof kv) {
        snprintf(v, sizeof v, "%s (%s), built %s", kv.version, kv.build_id, kv.stamp);
        add(g_sw, &g_sw_n, "Kernel", v);
    }
    snprintf(v, sizeof v, "%s, built %s", TOYOS_VERSION_FULL, TOYOS_BUILD_DATE);
    add(g_sw, &g_sw_n, "Userland", v);
    add(g_sw, &g_sw_n, "Boot", "GRUB, Multiboot2");
    struct query_fsinfo fs;
    if (sys_query_record(QUERY_FSINFO, 0, &fs, sizeof fs) >= (int)sizeof fs &&
        (fs.flags & QUERY_FS_MOUNTED)) {
        human(fs.used_bytes, a, sizeof a);
        human(fs.total_bytes, b, sizeof b);
        snprintf(v, sizeof v, "%s -- %s used of %s", fs.name, a, b);
        add(g_sw, &g_sw_n, "Root filesystem", v);
    }
    add(g_sw, &g_sw_n, "Up for", "-");

    struct query_smbios sm;
    if (sys_query_record(QUERY_SMBIOS, 0, &sm, sizeof sm) >= (int)sizeof sm && sm.found) {
        snprintf(v, sizeof v, "%s%s%s", sm.sys_vendor,
                 sm.sys_vendor[0] && sm.product[0] ? " " : "", sm.product);
        add(g_hw, &g_hw_n, "Machine", v);
        snprintf(v, sizeof v, "%s%s%s%s%s", sm.bios_vendor, sm.bios_vendor[0] ? " " : "",
                 sm.bios_version, sm.bios_date[0] ? ", " : "", sm.bios_date);
        add(g_hw, &g_hw_n, "Firmware", v);
    }

    struct cpu_info cpu;
    sys_cpu_info(&cpu);
    const char *brand = cpu.brand;
    while (*brand == ' ') brand++;
    int threads = 0;
    struct query_cpu qc;
    QUERY_FOREACH(QUERY_CPUS, qc, i) if (qc.flags & QUERY_CPU_ENABLED) threads++;
    if (threads > 1) snprintf(v, sizeof v, "%s -- %d threads", *brand ? brand : cpu.vendor, threads);
    else snprintf(v, sizeof v, "%s", *brand ? brand : cpu.vendor);
    add(g_hw, &g_hw_n, "Processor", v);
    add(g_hw, &g_hw_n, "Memory", "-");

    static struct udevice dev[UDEV_MAX];
    int nd = udevice_list(dev, UDEV_MAX);
    for (int i = 0; i < nd; i++)
        if (dev[i].type == UDEV_T_DISPLAY && dev[i].driver[0]) {
            snprintf(v, sizeof v, "%s (%s)", dev[i].name, dev[i].driver);
            add(g_hw, &g_hw_n, "Graphics", v);
            break;
        }
    struct query_display qd;
    if (sys_query_record(QUERY_DISPLAY, 0, &qd, sizeof qd) >= (int)sizeof qd && qd.width) {
        if (qd.refresh_mhz)
            snprintf(v, sizeof v, "%s%s%llu x %llu at %llu Hz", qd.panel, qd.panel[0] ? ", " : "",
                     (unsigned long long)qd.width, (unsigned long long)qd.height,
                     (unsigned long long)((qd.refresh_mhz + 500) / 1000));
        else
            snprintf(v, sizeof v, "%s%s%llu x %llu", qd.panel, qd.panel[0] ? ", " : "",
                     (unsigned long long)qd.width, (unsigned long long)qd.height);
        add(g_hw, &g_hw_n, "Display", v);
    }
    struct query_sound qs;
    QUERY_FOREACH(QUERY_SOUND, qs, j) if (qs.active) { add(g_hw, &g_hw_n, "Sound", qs.label); break; }
    snprintf(v, sizeof v, "%s%s%s", (cpu.enabled & CPU_EN_NX) ? "NX " : "",
             (cpu.enabled & CPU_EN_SMEP) ? "SMEP " : "", (cpu.enabled & CPU_EN_SMAP) ? "SMAP " : "");
    if (v[0]) { v[strlen(v) - 1] = '\0'; strlcat(v, " on", sizeof v); }
    add(g_hw, &g_hw_n, "Protections", v);
    g_loaded_once = 1;
    live_rows();
}

// Returns 1 once a minute, when the live rows are re-read -- uptime is
// shown in minutes, so a faster repaint would show nothing new.
int sysinfo_tick(void) {
    static uint64_t last = ~0ull;
    uint64_t min = sys_monotonic_ns() / 60000000000ull;
    if (!g_loaded_once || min == last) return 0;
    last = min;
    live_rows();
    return 1;
}

// --- geometry, shared by the measure and the draw -------------------------

static int line_h(void) { return ugfx_char_h() + 8; }
static int pad(void)    { return ugfx_char_w(); }
static int logo(void)   { return ugfx_char_h() * 4; }
static int caption_h(void) { return ugfx_char_h() + 10; }
static int card_h(int n) { return n * line_h() + pad(); }

static int key_w(void) {
    int w = 0;
    for (int i = 0; i < g_sw_n; i++) { int t = ugfx_text_width(g_sw[i].key); if (t > w) w = t; }
    for (int i = 0; i < g_hw_n; i++) { int t = ugfx_text_width(g_hw[i].key); if (t > w) w = t; }
    return w + pad() * 2;
}

int sysinfo_height(void) {
    return pad() + logo() + ugfx_char_h() * 3 + pad() +
           caption_h() + card_h(g_sw_n) + caption_h() + card_h(g_hw_n) + pad();
}

static int draw_card(struct ugfx_surface *s, int x, int y, int w, const char *title,
                     const struct si_row *rows, int n) {
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    ugfx_draw_string_clipped(s, x + 2, y + 4, w, title, dim, UTHEME_PANEL_BG);
    y += caption_h();
    int h = card_h(n);
    uui_fill_round_rect(s, x, y, w, h, 6, UTHEME_SEPARATOR);
    uui_fill_round_rect(s, x + 1, y + 1, w - 2, h - 2, 5, UTHEME_WHITE);
    int kw = key_w();
    for (int i = 0; i < n; i++) {
        int ry = y + pad() / 2 + i * line_h() + (line_h() - ugfx_char_h()) / 2;
        ugfx_draw_string_clipped(s, x + pad(), ry, kw - pad(), rows[i].key, dim, UTHEME_WHITE);
        ugfx_draw_string_clipped(s, x + kw, ry, w - kw - pad(), rows[i].val, UTHEME_TEXT, UTHEME_WHITE);
    }
    return y + h;
}

static void draw_view(struct ugfx_surface *s, const struct uui_custom *c) {
    int x = c->x, y = c->y + pad(), w = c->w;
    // The mark and the name, centred -- the one place in Settings that is
    // about the system rather than a setting of it.
    const struct uimg *ic = icon_get("toyos", logo());
    if (ic) ugfx_blit_alpha(s, x + (w - ic->w) / 2, y, ic->w, ic->h, ic->px, ic->w);
    y += logo() + 4;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    int tw = ugfx_text_width(g_title);
    ugfx_draw_string_clipped(s, x + (w - tw) / 2, y, w, g_title, UTHEME_TEXT, UTHEME_PANEL_BG);
    ugfx_set_font(was);
    y += ugfx_char_h() + 4;
    const char *tag = "An x86-64 hobby operating system";
    tw = ugfx_text_width(tag);
    ugfx_draw_string_clipped(s, x + (w - tw) / 2, y, w, tag,
                             uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), UTHEME_PANEL_BG);
    y += ugfx_char_h() * 2;
    y = draw_card(s, x, y, w, "Software", g_sw, g_sw_n);
    draw_card(s, x, y, w, "Hardware", g_hw, g_hw_n);
}

void sysinfo_init(void) {
    g_si_view = (struct uui_custom){ .draw = draw_view };
    uui_button_init(&g_si_devmgr, 0, 0, 0, 0, "Open Device Manager", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_SI_DEVMGR);
    uui_button_init(&g_si_copy, 0, 0, 0, 0, "Copy to clipboard", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_SI_COPY);
    uui_checkbox_init(&g_si_debug_cb, 0, 0, 0, 0, UTHEME_WHITE, UTHEME_TEXT);
    uui_setting_row_init(&g_si_debug, "Kernel and debugging settings",
                         "List the kernel and diagnostics pages in the sidebar",
                         (struct uui_item){ .ops = &uui_checkbox_ops, .widget = &g_si_debug_cb,
                                            .id = ID_SI_DEBUG, .name = "si_debug" });
}

// "Key: value" lines, as KDE's Copy to Clipboard gives them.
int sysinfo_copy(void) {
    static char text[2048];
    int n = snprintf(text, sizeof text, "%s\n", g_title);
    for (int i = 0; i < g_sw_n && n < (int)sizeof text; i++)
        n += snprintf(text + n, sizeof text - n, "%s: %s\n", g_sw[i].key, g_sw[i].val);
    for (int i = 0; i < g_hw_n && n < (int)sizeof text; i++)
        n += snprintf(text + n, sizeof text - n, "%s: %s\n", g_hw[i].key, g_hw[i].val);
    if (n >= (int)sizeof text) return 0;
    return uclip_set_text(text, n);
}
