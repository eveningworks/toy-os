// See uargs.h.
#include "lib/uargs.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"

#define WIDTH   79   // GNU's help does not follow the terminal either
#define COL_MAX 30   // the help column, at most; a longer left side wraps

static int claims(const struct uargs_prog *p, char c) {
    for (const struct uargs_opt *o = p->opts; o && (o->name || o->shortc); o++)
        if (o->shortc == c) return 1;
    return 0;
}

int uargs_error(const struct uargs_prog *p, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    char line[384];
    snprintf(line, sizeof line, "%s: %s\nTry '%s --help' for more information.\n", p->name, msg, p->name);
    sys_eprint(line);
    return UARGS_USAGE;
}

// --- the page ----------------------------------------------------------------

static void out(const char *s) { sys_print(s); }

// `text` from column `col` (the first line already positioned there),
// broken at spaces to fit WIDTH.
static void wrapped(const char *text, int col) {
    int room = WIDTH - col;
    if (room < 20) room = 20;
    char line[WIDTH + 2];
    while (*text) {
        int n = (int)strlen(text);
        if (n > room) {
            n = room;
            while (n > 0 && text[n] != ' ') n--;
            if (n == 0) n = room;
        }
        snprintf(line, sizeof line, "%.*s\n", n, text);
        out(line);
        text += n;
        while (*text == ' ') text++;
        if (*text) {
            snprintf(line, sizeof line, "%*s", col, "");
            out(line);
        }
    }
}

// One "  left    help" row, the help in column `col`.
static void row(const char *left, const char *help, int col) {
    char buf[160];
    int n = snprintf(buf, sizeof buf, "  %s", left);
    out(buf);
    if (!help || !*help) { out("\n"); return; }
    if (n + 2 > col) {
        snprintf(buf, sizeof buf, "\n%*s", col, "");
        out(buf);
    } else {
        snprintf(buf, sizeof buf, "%*s", col - n, "");
        out(buf);
    }
    wrapped(help, col);
}

static void opt_left(const struct uargs_opt *o, char *buf, int cap) {
    char lng[96] = "";
    if (o->name)
        snprintf(lng, sizeof lng, "--%s%s%s", o->name, o->arg ? "=" : "", o->arg ? o->arg : "");
    if (o->shortc && o->name) snprintf(buf, (size_t)cap, "-%c, %s", o->shortc, lng);
    else if (o->shortc) snprintf(buf, (size_t)cap, "-%c%s%s", o->shortc, o->arg ? " " : "", o->arg ? o->arg : "");
    else snprintf(buf, (size_t)cap, "    %s", lng);
}

static void cmd_left(const struct uargs_cmd *c, char *buf, int cap) {
    snprintf(buf, (size_t)cap, "%s%s%s", c->name, c->args ? " " : "", c->args ? c->args : "");
}

void uargs_help(const struct uargs_prog *p) {
    char buf[256];
    // The Usage line, then each further form as "  or:  ".
    const char *u = p->usage ? p->usage : "";
    for (int first = 1; ; first = 0) {
        const char *nl = strchr(u, '\n');
        int n = nl ? (int)(nl - u) : (int)strlen(u);
        snprintf(buf, sizeof buf, "%s %s %.*s\n", first ? "Usage:" : "  or: ", p->name, n, u);
        out(buf);
        if (!nl) break;
        u = nl + 1;
    }
    if (p->summary) { out(p->summary); out("\n"); }

    // One column for both tables, from the longest left side that fits.
    int col = 0;
    for (const struct uargs_cmd *c = p->cmds; c && (c->name || c->help); c++) {
        if (!c->name) continue;
        cmd_left(c, buf, sizeof buf);
        int w = (int)strlen(buf) + 4;
        if (w > col && w <= COL_MAX) col = w;
    }
    for (const struct uargs_opt *o = p->opts; o && (o->name || o->shortc); o++) {
        opt_left(o, buf, sizeof buf);
        int w = (int)strlen(buf) + 4;
        if (w > col && w <= COL_MAX) col = w;
    }
    if (col < 16) col = 16;

    if (p->cmds) {
        int headed = 0;
        for (const struct uargs_cmd *c = p->cmds; c->name || c->help; c++) {
            if (!c->name) {
                snprintf(buf, sizeof buf, "\n%s:\n", c->help);
                out(buf);
                headed = 1;
                continue;
            }
            if (!headed) { out("\nCommands:\n"); headed = 1; }
            cmd_left(c, buf, sizeof buf);
            row(buf, c->help, col);
        }
    }

    out("\nOptions:\n");
    for (const struct uargs_opt *o = p->opts; o && (o->name || o->shortc); o++) {
        opt_left(o, buf, sizeof buf);
        row(buf, o->help, col);
    }
    row(claims(p, 'h') ? "    --help" : "-h, --help", "show this help and exit", col);

    if (p->notes) { out("\n"); out(p->notes); out("\n"); }
    snprintf(buf, sizeof buf, "\nFull manual: doc %s\n", p->name);
    out(buf);
    if (p->more) { out("\n"); out(p->more()); }
}

// --- parsing -----------------------------------------------------------------

static int distance(const char *a, const char *b) {
    int na = (int)strlen(a), nb = (int)strlen(b), row_[40];
    if (na >= 40 || nb >= 40) return 99;
    for (int j = 0; j <= nb; j++) row_[j] = j;
    for (int i = 1; i <= na; i++) {
        int diag = row_[0];
        row_[0] = i;
        for (int j = 1; j <= nb; j++) {
            int up = row_[j], best = diag + (a[i - 1] != b[j - 1]);
            if (row_[j - 1] + 1 < best) best = row_[j - 1] + 1;
            if (up + 1 < best) best = up + 1;
            row_[j] = best;
            diag = up;
        }
    }
    return row_[nb];
}

static const struct uargs_opt *by_long(const struct uargs_prog *p, const char *name, int len) {
    for (const struct uargs_opt *o = p->opts; o && (o->name || o->shortc); o++)
        if (o->name && (int)strlen(o->name) == len && !strncmp(o->name, name, (size_t)len)) return o;
    return 0;
}

static const struct uargs_opt *by_short(const struct uargs_prog *p, char c) {
    for (const struct uargs_opt *o = p->opts; o && (o->name || o->shortc); o++)
        if (o->shortc == c) return o;
    return 0;
}

static void set(const struct uargs_opt *o, int pos, const char *val) {
    if (o->flag) *o->flag = pos;
    if (o->value && val) *o->value = val;
}

int uargs_parse(struct uargs *a, const struct uargs_prog *p, int argc, char **argv) {
    int npos = 0, opts_done = 0;
    a->status = 0;
    for (int i = 1; i < argc; i++) {
        char *s = argv[i];
        int shorts_ok = !opts_done && !(p->cmds && npos > 0);
        if (!opts_done && !strcmp(s, "--")) { opts_done = 1; continue; }
        if (!opts_done && s[0] == '-' && s[1] == '-') {
            const char *name = s + 2, *eq = strchr(name, '=');
            int len = eq ? (int)(eq - name) : (int)strlen(name);
            if (len == 4 && !strncmp(name, "help", 4)) { uargs_help(p); return 1; }
            const struct uargs_opt *o = by_long(p, name, len);
            if (!o) { a->status = uargs_error(p, "unknown option '%s'", s); return 1; }
            if (!o->arg && eq) { a->status = uargs_error(p, "option '--%s' takes no value", o->name); return 1; }
            const char *val = eq ? eq + 1 : 0;
            if (o->arg && !val) {
                if (i + 1 >= argc) { a->status = uargs_error(p, "option '--%s' needs a value (--%s=%s)", o->name, o->name, o->arg); return 1; }
                val = argv[++i];
            }
            set(o, i, val);
            continue;
        }
        if (shorts_ok && s[0] == '-' && s[1]) {
            for (int j = 1; s[j]; j++) {
                if (s[j] == 'h' && !claims(p, 'h')) { uargs_help(p); return 1; }
                const struct uargs_opt *o = by_short(p, s[j]);
                if (!o) { a->status = uargs_error(p, "unknown option '-%c'", s[j]); return 1; }
                if (!o->arg) { set(o, i, 0); continue; }
                // A value: the rest of this word, or the next one.
                const char *val = s[j + 1] ? s + j + 1 : (i + 1 < argc ? argv[++i] : 0);
                if (!val) { a->status = uargs_error(p, "option '-%c' needs a value (%s)", o->shortc, o->arg); return 1; }
                set(o, i, val);
                break;
            }
            continue;
        }
        argv[npos++] = s;
        if (p->first_operand_ends_options) opts_done = 1;
    }
    a->argc = npos;
    a->argv = argv;
    if (p->cmds && npos > 0) {
        const char *best = 0;
        int best_d = 3;
        for (const struct uargs_cmd *c = p->cmds; c->name || c->help; c++) {
            if (!c->name) continue;
            if (!strcmp(c->name, argv[0])) return 0;
            int d = distance(argv[0], c->name);
            if (d < best_d) { best_d = d; best = c->name; }
        }
        if (best) a->status = uargs_error(p, "unknown command '%s' -- did you mean '%s'?", argv[0], best);
        else a->status = uargs_error(p, "unknown command '%s'", argv[0]);
        return 1;
    }
    return 0;
}
