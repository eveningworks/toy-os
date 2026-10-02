// The Terminal's Session panel: what the selected tab's shell is, the
// processes under it, how much scrollback it holds, and three signals.
// Its own file because none of it touches the grid; terminal.c hands it
// a term_panel_info and routes its three buttons.
//
// **THE PROCESS TREE IS READ ON EVERY PAINT** -- SYS_PROC_INFO over the
// 64-slot table, the snapshot ps takes. Nothing announces that a job
// started or stopped, so the app repaints on its tick while the panel is
// open (terminal.c's on_tick).
#include "term.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "proc_info.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_button.h"
#include "ui/uui_primitives.h"

#define TREE_ROWS 5   // shown; the rest collapse into "and N more"

static struct uui_button g_btn[TERM_PANEL_BUTTONS];
static struct uui_item g_items[TERM_PANEL_BUTTONS];
static int g_x, g_y, g_w, g_h;

static int pad(void)   { return ugfx_char_w(); }
static int line_h(void) { return ugfx_char_h() + ugfx_char_h() / 3; }

int term_panel_width(void) { return ugfx_char_advance('n') * 32; }

void term_panel_init(void) {
    static const char *const labels[TERM_PANEL_BUTTONS] = {
        "Interrupt", "Send EOF", "Force quit shell",
    };
    static const char *const names[TERM_PANEL_BUTTONS] = {
        "panel-intr", "panel-eof", "panel-kill",
    };
    for (int i = 0; i < TERM_PANEL_BUTTONS; i++) {
        uui_button_init(&g_btn[i], 0, 0, 0, 0, labels[i], UTHEME_BUTTON_BG, UTHEME_TEXT, i);
        g_items[i] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_btn[i],
                                        .id = TERM_PANEL_ID_BASE + i, .name = names[i] };
    }
    // Force quit is the one that loses work: outlined in the danger role.
    g_btn[TERM_PANEL_KILL].outlined = 1;
    g_btn[TERM_PANEL_KILL].fg = utheme_action(UTHEME_ACT_DANGER);
}

struct uui_item *term_panel_item(int i) {
    return (i >= 0 && i < TERM_PANEL_BUTTONS) ? &g_items[i] : 0;
}

// A section caption takes a third of a line of air above it and a line.
static int caption_h(void) { return line_h() + line_h() / 3; }

// What the draw puts above the buttons: the title, five SHELL rows,
// TREE_ROWS, the meter and its readout, and four captions. ONE function,
// so the buttons are placed where the draw leaves room for them.
static int buttons_top(void) {
    return g_y + pad() + (1 + 5 + TREE_ROWS + 2) * line_h() + 4 * caption_h();
}

void term_panel_layout(int x, int y, int w, int h) {
    g_x = x; g_y = y; g_w = w; g_h = h;
    int bh = utheme_control_h();
    int by = buttons_top();
    for (int i = 0; i < TERM_PANEL_BUTTONS; i++) {
        uui_button_set_geometry(&g_btn[i], x + pad(), by, w - 2 * pad(), bh);
        by += bh + ugfx_char_h() / 3;
    }
}

void term_panel_set_exited(int exited) {
    for (int i = 0; i < TERM_PANEL_BUTTONS; i++) g_btn[i].disabled = exited;
}

static void caption(struct ugfx_surface *s, int *y, const char *text, int role) {
    *y += caption_h() - line_h();
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string(s, g_x + pad(), *y, text, utheme_action(role), UTHEME_PANEL_BG);
    ugfx_set_font(was);
    *y += line_h();
}

static void keyval(struct ugfx_surface *s, int *y, const char *k, const char *v) {
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    int kw = ugfx_char_advance('n') * 10;
    ugfx_draw_string(s, g_x + pad(), *y, k, dim, UTHEME_PANEL_BG);
    ugfx_draw_string_elided(s, g_x + pad() + kw, *y, g_w - 2 * pad() - kw, v,
                            UTHEME_TEXT, UTHEME_PANEL_BG);
    *y += line_h();
}

// What a process is doing, in words a person would use.
static const char *doing(const struct proc_info *p) {
    switch (p->state) {
    case PROC_STATE_RUNNING:
    case PROC_STATE_READY:   return "running";
    case PROC_STATE_STOPPED: return "stopped";
    case PROC_STATE_ZOMBIE:  return "exited";
    case PROC_STATE_BLOCKED:
        switch (p->wait_reason) {
        case PROC_WAIT_KEY:   return "reading";
        case PROC_WAIT_CHILD: return "waiting for a job";
        case PROC_WAIT_TIMER: return "sleeping";
        case PROC_WAIT_DISK:  return "on disk";
        default:              return "waiting";
        }
    default: return "";
    }
}

// The shell and everything under it, depth-first, threads hidden -- ps
// --tree's walk, cut to what fits.
static struct proc_info *g_procs;   // sys_proc_max() of them, sized on first draw
static int g_procs_cap, g_nprocs;

static void tree_row(struct ugfx_surface *s, int *y, const struct proc_info *p, int depth,
                     int sel) {
    char pid[12];
    snprintf(pid, sizeof pid, "%d", (int)p->pid);
    int x = g_x + pad();
    if (sel)
        uui_fill_round_rect(s, x - pad() / 2, *y - 1, g_w - pad(), line_h(),
                            ugfx_char_h() / 4, UTHEME_SELECTION);
    uint32_t bg = sel ? UTHEME_SELECTION : UTHEME_PANEL_BG;
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    ugfx_draw_string(s, x, *y, pid, dim, bg);
    int nx = x + ugfx_char_advance('0') * 4 + depth * ugfx_char_advance('n') * 2;
    const char *what = doing(p);
    int ww = ugfx_text_width(what);
    ugfx_draw_string_elided(s, nx, *y, g_x + g_w - pad() - ww - ugfx_char_w() - nx, p->name,
                            UTHEME_TEXT, bg);
    ugfx_draw_string(s, g_x + g_w - pad() - ww, *y, what, dim, bg);
    *y += line_h();
}

static int walk(struct ugfx_surface *s, int *y, int pid, int depth, int *shown, int *hidden) {
    for (int i = 0; i < g_nprocs; i++) {
        if (g_procs[i].ppid != pid) continue;
        if (*shown < TREE_ROWS) { tree_row(s, y, &g_procs[i], depth, 0); (*shown)++; }
        else (*hidden)++;
        walk(s, y, g_procs[i].pid, depth + 1, shown, hidden);
    }
    return *shown;
}

void term_panel_draw(struct ugfx_surface *s, const struct term_panel_info *in) {
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
    ugfx_fill_rect(s, g_x, g_y, g_w, g_h, UTHEME_PANEL_BG);
    ugfx_fill_rect(s, g_x, g_y, 1, g_h, UTHEME_SEPARATOR);

    int y = g_y + pad();
    const struct ugfx_font *b = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string(s, g_x + pad(), y, "Session", UTHEME_TEXT, UTHEME_PANEL_BG);
    ugfx_set_font(b);
    y += line_h();

    char v[64];
    caption(s, &y, "SHELL", UTHEME_ACT_NAV);
    keyval(s, &y, "Program", in->shell);
    if (in->exited) strlcpy(v, "exited", sizeof v);
    else snprintf(v, sizeof v, "pid %d, group %d", in->pid, in->pid);
    keyval(s, &y, "Process", v);
    keyval(s, &y, "Directory", in->cwd[0] ? in->cwd : "unknown");
    snprintf(v, sizeof v, "%d x %d", in->cols, in->rows);
    keyval(s, &y, "Size", v);
    keyval(s, &y, "Scheme", in->scheme);

    caption(s, &y, "PROCESSES", UTHEME_ACT_VIEW);
    int tree_end = y + TREE_ROWS * line_h();
    g_nprocs = 0;
    if (!g_procs && (g_procs_cap = sys_proc_max()) > 0)
        g_procs = malloc((size_t)g_procs_cap * sizeof *g_procs);
    struct proc_info info;
    for (int i = 0; g_procs && g_nprocs < g_procs_cap && sys_proc_info(i, &info) == 0; i++) {
        if (info.pid == 0) continue;
        if (info.tgid != info.pid) continue;
        g_procs[g_nprocs++] = info;
    }
    int shown = 0, hidden = 0;
    for (int i = 0; i < g_nprocs && !in->exited; i++) {
        if (g_procs[i].pid != in->pid) continue;
        tree_row(s, &y, &g_procs[i], 0, 1);
        shown = 1;
        walk(s, &y, in->pid, 1, &shown, &hidden);
        break;
    }
    if (hidden) {
        snprintf(v, sizeof v, "and %d more", hidden);
        ugfx_draw_string(s, g_x + pad(), y, v, uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED),
                         UTHEME_PANEL_BG);
    }
    y = tree_end;

    caption(s, &y, "SCROLLBACK", UTHEME_ACT_VIEW);
    int mw = g_w - 2 * pad(), mh = ugfx_char_h() / 3 + 2;
    uui_fill_round_rect(s, g_x + pad(), y + mh, mw, mh, UUI_CAPSULE, UTHEME_TAB_REST);
    int fill = in->sb_cap > 0 ? (int)((long)mw * in->sb_count / in->sb_cap) : 0;
    if (fill > 0)
        uui_fill_round_rect(s, g_x + pad(), y + mh, fill < mh ? mh : fill, mh, UUI_CAPSULE,
                            UTHEME_ACCENT);
    y += line_h();
    snprintf(v, sizeof v, "%d of %d", in->sb_count, in->sb_cap);
    keyval(s, &y, "Lines kept", v);

    caption(s, &y, "SIGNALS", UTHEME_ACT_DANGER);
    ugfx_set_font(was);
}
