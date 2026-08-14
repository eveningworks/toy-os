// Task Manager: lists open windows and shows system memory usage. See
// taskmgr.h's top comment for scope -- windows + memory only, no CPU
// column (toy-os's GUI apps aren't scheduled processes, so there's no
// real per-app CPU number to show yet; see docs/decisions.md).
//
// Read-only, no state of its own to kzalloc/kfree -- every number is
// pulled fresh from wm.h/kapi.h on each on_draw() call, same
// "derive live, don't persist a stale answer" approach the Start
// menu's hover highlight already uses. Not multi_instance: a second
// Task Manager window would show identical information to the first,
// so there's nothing multiple windows would add (same reasoning as
// About).
#include "taskmgr.h"
#include "wm/wm.h"
#include "theme.h"
#include "kapi.h"

#define TM_MARGIN 8
#define TM_LINE_GAP 6
#define TM_COLS 40                // widest line's character budget
#define TM_MAX_WINDOW_ROWS 6      // matches apps/wm/wm_internal.h's MAX_WINDOWS
#define TM_HEADER_ROWS 2          // "Windows:" + a blank separator before the memory panel
#define TM_MEMORY_ROWS 5          // section title + 4 stat lines (see taskmgr_draw)

void taskmgr_default_size(int *w, int *h) {
    int rows = TM_HEADER_ROWS + TM_MAX_WINDOW_ROWS + 1 /* blank line */ + TM_MEMORY_ROWS;
    *w = 2 * TM_MARGIN + TM_COLS * gfx_char_w();
    *h = 2 * TM_MARGIN + rows * gfx_char_h() + (rows - 1) * TM_LINE_GAP;
}

void taskmgr_open(struct window *win) {
    (void)win; // no per-window state -- everything is read fresh on draw
}

// ---- tiny local number formatting -- this kernel has no printf, and
// pulling in a general one just for this app isn't worth it (see
// widgets.h/theme.h's own "stay minimal" precedent) ----

static int u64_to_str(uint64_t v, char *buf) {
    char tmp[20];
    int n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

// Formats a byte count as whichever of B/KB/MB is most readable --
// truncated (not rounded) division, same "good enough for a live
// glance, not a precise report" spirit as the rest of this app.
static void format_bytes(uint64_t bytes, char *out) {
    char num[20];
    int i = 0;
    if (bytes >= 1024 * 1024) {
        u64_to_str(bytes / (1024 * 1024), num);
        for (int j = 0; num[j]; j++) out[i++] = num[j];
        out[i++] = ' '; out[i++] = 'M'; out[i++] = 'B';
    } else if (bytes >= 1024) {
        u64_to_str(bytes / 1024, num);
        for (int j = 0; num[j]; j++) out[i++] = num[j];
        out[i++] = ' '; out[i++] = 'K'; out[i++] = 'B';
    } else {
        u64_to_str(bytes, num);
        for (int j = 0; num[j]; j++) out[i++] = num[j];
        out[i++] = ' '; out[i++] = 'B';
    }
    out[i] = '\0';
}

static const char *window_state_label(enum window_state st) {
    switch (st) {
        case WIN_NORMAL: return "normal";
        case WIN_MINIMIZED: return "minimized";
        case WIN_MAXIMIZED: return "maximized";
        default: return "?";
    }
}

void taskmgr_draw(struct window *win) {
    int cx = window_content_x(win);
    int cy = window_content_y(win);
    int cw = window_content_w(win);
    int ch = window_content_h(win);

    uint32_t bg = THEME_WINDOW_BG;
    uint32_t fg = THEME_TEXT;
    gfx_fill_rect(cx, cy, cw, ch, bg);

    int line_h = gfx_char_h() + TM_LINE_GAP;
    int row = 0;
    int x = cx + TM_MARGIN;
    int y = cy + TM_MARGIN;

    gfx_draw_string(x, y + (row++) * line_h, "Windows:  [r0] kernel-space  [r3] ring-3 process", fg, bg);

    int count = wm_window_count();
    for (int i = 0; i < TM_MAX_WINDOW_ROWS; i++) {
        if (i >= count) break;
        const struct window *w = wm_get_window(i);
        if (!w) break;

        char line[80];
        int p = 0;
        line[p++] = ' '; line[p++] = ' ';

        // Which side of the privilege boundary this window's code runs
        // on. Worth showing now that both kinds are ordinary desktop
        // windows and look identical: a ring-3 window is a separate
        // PROCESS the WM composites, a ring-0 one is a gui_app compiled
        // into the kernel. `client_pid` is exactly that distinction
        // (wm.h -- a window has an `app` or a client, never both), so
        // this reads the real state rather than a label anyone has to
        // remember to keep in step.
        const char *ring = w->client_pid ? "[r3] " : "[r0] ";
        for (int k = 0; ring[k] && p < 60; k++) line[p++] = ring[k];

        const char *title = w->title[0] ? w->title : "(untitled)";
        for (int k = 0; title[k] && p < 60; k++) line[p++] = title[k];
        line[p++] = ' '; line[p++] = '-'; line[p++] = ' ';
        const char *state = window_state_label(w->state);
        for (int k = 0; state[k] && p < 78; k++) line[p++] = state[k];

        // A ring-3 window's owning pid, so two clients of the same app
        // are tellable apart -- which a title alone cannot do.
        if (w->client_pid && p < 72) {
            line[p++] = ' '; line[p++] = '(';
            line[p++] = 'p'; line[p++] = 'i'; line[p++] = 'd'; line[p++] = ' ';
            int v = w->client_pid;
            char d[8];
            int dn = 0;
            if (v == 0) d[dn++] = '0';
            while (v > 0 && dn < 7) { d[dn++] = (char)('0' + v % 10); v /= 10; }
            while (dn > 0 && p < 77) line[p++] = d[--dn];
            line[p++] = ')';
        }
        line[p] = '\0';

        gfx_draw_string(x, y + (row++) * line_h, line, fg, bg);
    }
    if (count == 0) {
        gfx_draw_string(x, y + (row++) * line_h, "  (none)", fg, bg);
        row = TM_HEADER_ROWS - 1 + TM_MAX_WINDOW_ROWS; // keep the memory panel's y fixed either way
    } else {
        row = TM_HEADER_ROWS - 1 + TM_MAX_WINDOW_ROWS;
    }

    row++; // blank separator line

    gfx_draw_string(x, y + (row++) * line_h, "Memory:", fg, bg);

    uint64_t total_frames = pmm_total_frames();
    uint64_t free_frames = pmm_free_frames();
    uint64_t used_frames = total_frames - free_frames;

    char numbuf[24];
    char line[80];
    int p;

    format_bytes(total_frames * 4096, numbuf);
    p = 0; line[p++]=' '; line[p++]=' ';
    { const char *s = "RAM total: "; for (int k=0; s[k]; k++) line[p++]=s[k]; }
    { const char *s = numbuf; for (int k=0; s[k]; k++) line[p++]=s[k]; }
    line[p] = '\0';
    gfx_draw_string(x, y + (row++) * line_h, line, fg, bg);

    format_bytes(used_frames * 4096, numbuf);
    p = 0; line[p++]=' '; line[p++]=' ';
    { const char *s = "RAM used:  "; for (int k=0; s[k]; k++) line[p++]=s[k]; }
    { const char *s = numbuf; for (int k=0; s[k]; k++) line[p++]=s[k]; }
    line[p] = '\0';
    gfx_draw_string(x, y + (row++) * line_h, line, fg, bg);

    format_bytes(heap_total_bytes(), numbuf);
    p = 0; line[p++]=' '; line[p++]=' ';
    { const char *s = "Heap total: "; for (int k=0; s[k]; k++) line[p++]=s[k]; }
    { const char *s = numbuf; for (int k=0; s[k]; k++) line[p++]=s[k]; }
    line[p] = '\0';
    gfx_draw_string(x, y + (row++) * line_h, line, fg, bg);

    format_bytes(heap_used_bytes(), numbuf);
    p = 0; line[p++]=' '; line[p++]=' ';
    { const char *s = "Heap used:  "; for (int k=0; s[k]; k++) line[p++]=s[k]; }
    { const char *s = numbuf; for (int k=0; s[k]; k++) line[p++]=s[k]; }
    line[p] = '\0';
    gfx_draw_string(x, y + (row++) * line_h, line, fg, bg);
}
