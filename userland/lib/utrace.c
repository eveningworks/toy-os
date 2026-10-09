// strace's records as text -- see utrace.h.
#include "lib/utrace.h"
#include "syscall_abi.h"    // SYS_O_*
#include "syscall_meta.h"
#include "rt/sys.h"         // sys_errname()
#include <stdio.h>
#include <string.h>

// THE KERNEL'S ROWS, without the handlers: a macro argument that is never
// used is never looked up, so `fn` names nothing here.
struct desc { const char *name; enum sc_arg args[3]; enum sc_ret ret; };
static const struct desc ROWS[] = {
#define SYSCALL_ROW(nr, name, fn, a0, a1, a2, r) [nr] = { name, { a0, a1, a2 }, r },
#include "syscall_rows.h"
#undef SYSCALL_ROW
};
#define NROWS ((int)(sizeof ROWS / sizeof ROWS[0]))

static const struct desc *row(int nr) {
    return nr >= 0 && nr < NROWS && ROWS[nr].name ? &ROWS[nr] : 0;
}

const char *utrace_name(int nr) {
    const struct desc *d = row(nr);
    return d ? d->name : 0;
}

int utrace_lookup(const char *name) {
    for (int i = 0; i < NROWS; i++)
        if (ROWS[i].name && !strcmp(ROWS[i].name, name)) return i;
    return -1;
}

// ---- appending, never past `cap` -----------------------------------------------

struct out { char *buf; size_t cap, len; };

static void put(struct out *o, const char *s) {
    while (*s && o->len + 1 < o->cap) o->buf[o->len++] = *s++;
    if (o->cap) o->buf[o->len < o->cap ? o->len : o->cap - 1] = 0;
}

static void putf(struct out *o, const char *fmt, long long v) {
    char t[32];
    snprintf(t, sizeof t, fmt, v);
    put(o, t);
}

// Real strace's escapes: the four common ones by name, printable ASCII as
// itself, anything else as \xNN -- so a line never carries a control
// character that would move a terminal's cursor.
static void put_quoted(struct out *o, const char *s, int n, int cut) {
    put(o, "\"");
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char t[8];
        if (c == '\n') put(o, "\\n");
        else if (c == '\t') put(o, "\\t");
        else if (c == '\r') put(o, "\\r");
        else if (c == '"') put(o, "\\\"");
        else if (c == '\\') put(o, "\\\\");
        else if (c >= 0x20 && c < 0x7f) { t[0] = (char)c; t[1] = 0; put(o, t); }
        else { snprintf(t, sizeof t, "\\x%02x", c); put(o, t); }
    }
    put(o, "\"");
    if (cut) put(o, "...");
}

static void put_oflags(struct out *o, uint64_t f) {
    static const struct { uint64_t bit; const char *name; } known[] = {
        { SYS_O_WRITE, "O_WRITE" }, { SYS_O_CREAT, "O_CREAT" }, { SYS_O_TRUNC, "O_TRUNC" },
    };
    if (!f) { put(o, "0"); return; }
    int first = 1;
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++) {
        if (!(f & known[i].bit)) continue;
        if (!first) put(o, "|");
        put(o, known[i].name);
        first = 0;
        f &= ~known[i].bit;
    }
    if (f) {   // an unknown bit -- shown rather than silently dropped
        if (!first) put(o, "|");
        putf(o, "0x%llx", (long long)f);
    }
}

size_t utrace_format_call(char *out, size_t cap, const struct trace_rec *e) {
    struct out o = { out, cap, 0 };
    if (cap) out[0] = 0;
    const struct desc *d = row(e->nr);
    if (d) put(&o, d->name);
    else putf(&o, "syscall_%lld", e->nr);
    put(&o, "(");
    for (int i = 0; i < 3; i++) {
        // A number with no row shows all three registers, as hex.
        enum sc_arg k = d ? d->args[i] : A_HEX;
        if (k == A_END) break;
        if (i) put(&o, ", ");
        uint64_t v = e->a[i];
        switch (k) {
        case A_INT: case A_FD: putf(&o, "%lld", (long long)(int64_t)v); break;
        case A_OFLAGS:         put_oflags(&o, v); break;
        case A_PATH: case A_BUF:
            // The bytes the kernel copied at the call; a pointer it could
            // not read is shown as the pointer.
            if (e->blob_arg == i) put_quoted(&o, e->blob, e->blob_len, e->blob_cut);
            else putf(&o, "0x%llx", (long long)v);
            break;
        default:               putf(&o, "0x%llx", (long long)v); break;
        }
    }
    put(&o, ")");
    return o.len;
}

int utrace_failed(const struct trace_rec *x) {
    if (x->kind != TRACE_EXIT && x->kind != TRACE_RESUMED) return 0;
    return x->ret < 0 && x->ret >= -4095;
}

size_t utrace_format_ret(char *out, size_t cap, int nr, const struct trace_rec *x) {
    struct out o = { out, cap, 0 };
    if (cap) out[0] = 0;
    if (x->kind == TRACE_NORETURN) { put(&o, " = ?"); return o.len; }
    const struct desc *d = row(nr);
    if (utrace_failed(x)) {
        putf(&o, " = %lld", x->ret);
        const char *name = sys_errname((int)-x->ret);
        if (name) { put(&o, " "); put(&o, name); }
    } else if (d && d->ret == R_HEX) {
        putf(&o, " = 0x%llx", x->ret);
    } else {
        putf(&o, " = %lld", x->ret);
    }
    return o.len;
}

// ---- pairing ------------------------------------------------------------------

void utrace_printer_init(struct utrace_printer *p, void (*emit)(void *, const char *), void *ctx) {
    memset(p, 0, sizeof *p);
    p->emit = emit;
    p->ctx = ctx;
}

static void emit_unfinished(struct utrace_printer *p) {
    char line[384];
    size_t n = utrace_format_call(line, sizeof line, &p->pending);
    snprintf(line + n, sizeof line - n, " <unfinished ...>");
    p->emit(p->ctx, line);
    p->have_pending = 0;
}

void utrace_feed(struct utrace_printer *p, const struct trace_rec *r) {
    char line[384];
    if (r->kind == TRACE_ENTRY) {
        if (p->have_pending) emit_unfinished(p);
        p->pending = *r;
        p->have_pending = 1;
        return;
    }
    // An exit: joined to its entry, or -- another call came between, a
    // second thread's -- written as Linux strace does, "resumed".
    if (p->have_pending && p->pending.nr == r->nr && p->pending.pid == r->pid) {
        size_t n = utrace_format_call(line, sizeof line, &p->pending);
        utrace_format_ret(line + n, sizeof line - n, r->nr, r);
        p->have_pending = 0;
    } else {
        const char *name = utrace_name(r->nr);
        size_t n = (size_t)snprintf(line, sizeof line, "<... %s resumed>", name ? name : "syscall");
        if (n < sizeof line) utrace_format_ret(line + n, sizeof line - n, r->nr, r);
    }
    p->emit(p->ctx, line);
}

void utrace_flush(struct utrace_printer *p) {
    if (p->have_pending) emit_unfinished(p);
}
