// See ubootcfg.h.
#include "lib/ubootcfg.h"
#include "lib/ubootwords.h"
#include <ctype.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "rt/sys.h"

static const char *skip_blank(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static int is_space(char ch) { return ch == ' ' || ch == '\t'; }

// One GRUB word, as ubootmenu.c reads it: quoted (double quotes take
// backslash escapes) or bare up to whitespace, `{` or `;`. 0 when it
// does not fit or a quote is never closed.
static int gword(const char **pp, const char *end, char *out, int cap) {
    const char *p = *pp;
    int n = 0;
    while (p < end && !is_space(*p) && *p != '{' && *p != ';') {
        char q = *p;
        if (q == '\'' || q == '"') {
            p++;
            while (p < end && *p != q) {
                if (q == '"' && *p == '\\' && p + 1 < end) p++;
                if (n >= cap - 1) return 0;
                out[n++] = *p++;
            }
            if (p >= end) return 0;
            p++;
        } else {
            if (n >= cap - 1) return 0;
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    *pp = p;
    return 1;
}

static int all_digits(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

static int starts_word(const char *p, const char *end, const char *w) {
    size_t n = strlen(w);
    return (size_t)(end - p) > n && !strncmp(p, w, n) && is_space(p[n]);
}

// May `len` bytes at `w` be edited as one plain word? No quoting or
// GRUB syntax -- except ONE `$name` running to the end of the word,
// grub.cfg's own `bootpart=$bootpart`: GRUB expands it whatever words
// sit beside it, so keeping it verbatim is all an edit has to do.
static int plain_word(const char *w, int len) {
    for (int i = 0; i < len; i++) {
        if ((unsigned char)w[i] <= ' ') return 0;
        if (w[i] == '$') {
            if (i + 1 >= len || !(isalpha((unsigned char)w[i + 1]) || w[i + 1] == '_')) return 0;
            for (int j = i + 1; j < len; j++)
                if (!(isalnum((unsigned char)w[j]) || w[j] == '_')) return 0;
            return 1;
        }
        if (strchr("\"';#{}\\", w[i])) return 0;
    }
    return len > 0;
}

// The entry's boot line: command, path, then words. `plain` only when
// nothing after the path needs GRUB's quoting or expansion beyond what
// plain_word() allows.
static void parse_boot(struct ubootcfg_entry *e, const char *p, const char *end) {
    p = skip_blank(p, end);
    while (p < end && !is_space(*p)) p++;          // the command
    p = skip_blank(p, end);
    int n = 0;
    while (p < end && !is_space(*p) && n < UBOOTCFG_PATH - 1) e->kernel[n++] = *p++;
    e->kernel[n] = 0;
    e->plain = n > 0 && (p >= end || is_space(*p));
    e->nwords = 0;
    while (e->plain) {
        p = skip_blank(p, end);
        if (p >= end) break;
        const char *w = p;
        while (p < end && !is_space(*p)) p++;
        int len = (int)(p - w);
        if (!plain_word(w, len)) e->plain = 0;
        if (!e->plain) break;
        if (e->nwords >= UBOOTCFG_WORDS || len >= UBOOTCFG_WORD) { e->plain = 0; break; }
        memcpy(e->word[e->nwords], w, (size_t)len);
        e->word[e->nwords][len] = 0;
        e->nwords++;
    }
}

int ubootcfg_parse(struct ubootcfg *c, const char *text, int len) {
    if (len < 0 || len >= UBOOTCFG_MAX) return -1;
    static char own[UBOOTCFG_MAX];
    if (text == c->text) text = memcpy(own, text, (size_t)len);
    memset(c, 0, sizeof *c);
    memcpy(c->text, text, (size_t)len);
    c->text[len] = 0;
    c->len = len;
    c->timeout = c->timeout_line = c->def = c->default_line = -1;
    c->open_line = c->stray_line = c->quote_line = -1;

    for (int off = 0; off < len; ) {
        if (c->nlines >= UBOOTCFG_LINES) return -1;
        c->line_at[c->nlines++] = off;
        const char *nl = memchr(c->text + off, '\n', (size_t)(len - off));
        off = nl ? (int)(nl - c->text) + 1 : len;
    }
    c->line_at[c->nlines] = len;

    int depth = 0, open_at[64], cur = -1;
    for (int ln = 0; ln < c->nlines; ln++) {
        const char *line = c->text + c->line_at[ln];
        const char *end = c->text + c->line_at[ln + 1];
        if (end > line && end[-1] == '\n') end--;
        const char *p = skip_blank(line, end);

        if (depth == 0 && starts_word(p, end, "menuentry")) {
            const char *t = skip_blank(p + 9, end);
            if (c->count < UBOOTMENU_MAX) {
                struct ubootcfg_entry *e = &c->entry[c->count];
                if (gword(&t, end, e->title, sizeof e->title)) {
                    e->line = ln;
                    e->end = -1;
                    e->boot = -1;
                    e->title_rest = (int)(t - line);
                    cur = c->count++;
                }
            } else {
                c->too_many = 1;
                cur = -1;
            }
        } else if (depth == 1 && cur >= 0 && c->entry[cur].boot < 0 &&
                   (starts_word(p, end, "multiboot2") || starts_word(p, end, "linux"))) {
            c->entry[cur].boot = ln;
            parse_boot(&c->entry[cur], p, end);
        } else if (depth == 0 && !strncmp(p, "set timeout=", 12)) {
            char v[16];
            const char *q = p + 12;
            c->timeout_line = ln;
            c->timeout = gword(&q, end, v, sizeof v) && all_digits(v) ? atoi(v) : -2;
        } else if (depth == 0 && !strncmp(p, "set default=", 12)) {
            // The one-shot stanza's `set default="${next_entry}"` is
            // computed at boot and says nothing about the default.
            char v[UBOOTMENU_TITLE];
            const char *q = p + 12;
            if (gword(&q, end, v, sizeof v) && !strchr(v, '$')) {
                c->default_line = ln;
                memcpy(c->defspec, v, sizeof v);
            }
        }

        char q = 0;
        for (const char *ch = line; ch < end; ch++) {
            if (q) {
                if (q == '"' && *ch == '\\' && ch + 1 < end) { ch++; continue; }
                if (*ch == q) q = 0;
                continue;
            }
            if (*ch == '#') break;
            if (*ch == '\'' || *ch == '"') q = *ch;
            else if (*ch == '{') {
                if (depth < 64) open_at[depth] = ln;
                depth++;
            } else if (*ch == '}') {
                if (depth == 0) { if (c->stray_line < 0) c->stray_line = ln; continue; }
                depth--;
                if (depth == 0 && cur >= 0 && c->entry[cur].end < 0) {
                    c->entry[cur].end = ln;
                    cur = -1;
                }
            }
        }
        // GRUB's quotes do span lines, but nothing in a grub.cfg this
        // edits does, and one left open swallows the rest of the file.
        if (q && c->quote_line < 0) c->quote_line = ln;
    }
    if (depth > 0) c->open_line = open_at[depth - 1 < 64 ? depth - 1 : 63];

    c->oneshot = strstr(c->text, "next_entry") != 0;
    c->def = c->defspec[0] ? ubootcfg_find(c, c->defspec) : (c->count ? 0 : -1);
    return 0;
}

int ubootcfg_load(struct ubootcfg *c, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    static char buf[UBOOTCFG_MAX + 1];
    int n = 0;
    for (;;) {
        long r = read(fd, buf + n, (size_t)(UBOOTCFG_MAX + 1 - n));
        if (r <= 0) break;
        n += (int)r;
        if (n > UBOOTCFG_MAX) break;
    }
    close(fd);
    if (n >= UBOOTCFG_MAX) return -2;
    return ubootcfg_parse(c, buf, n) < 0 ? -2 : 0;
}

void ubootcfg_line(const struct ubootcfg *c, int n, char *out, int cap) {
    out[0] = 0;
    if (n < 0 || n >= c->nlines || cap <= 0) return;
    int a = c->line_at[n], b = c->line_at[n + 1];
    if (b > a && c->text[b - 1] == '\n') b--;
    int len = b - a < cap - 1 ? b - a : cap - 1;
    memcpy(out, c->text + a, (size_t)len);
    out[len] = 0;
}

int ubootcfg_find(const struct ubootcfg *c, const char *spec) {
    if (!spec || !*spec) return -1;
    if (all_digits(spec)) {
        int n = atoi(spec);
        return n < c->count ? n : -1;
    }
    for (int i = 0; i < c->count; i++)
        if (!strcmp(c->entry[i].title, spec)) return i;
    return -1;
}

// Replace lines [first, first + nremove) with `repl` (whole lines, each
// ending in '\n', or ""), then reparse. A refused splice changes nothing.
static int splice(struct ubootcfg *c, int first, int nremove, const char *repl) {
    static char out[UBOOTCFG_MAX];
    int a = c->line_at[first], b = c->line_at[first + nremove];
    int rl = (int)strlen(repl);
    // The file's last line may lack its newline; text inserted after it
    // must not run on into it.
    int glue = a == c->len && a > 0 && c->text[a - 1] != '\n';
    int n = a + glue + rl + (c->len - b);
    if (n >= UBOOTCFG_MAX) { c->why = "the file would be too big"; return -1; }
    memcpy(out, c->text, (size_t)a);
    if (glue) out[a] = '\n';
    memcpy(out + a + glue, repl, (size_t)rl);
    memcpy(out + a + glue + rl, c->text + b, (size_t)(c->len - b));
    static struct ubootcfg tmp;
    if (ubootcfg_parse(&tmp, out, n) < 0) { c->why = "the file would be too big"; return -1; }
    memcpy(c, &tmp, sizeof tmp);
    return 0;
}

static int indent_of(const struct ubootcfg *c, int ln, char *out, int cap) {
    int n = 0;
    for (int i = c->line_at[ln]; i < c->line_at[ln + 1] && is_space(c->text[i]) && n < cap - 1; i++)
        out[n++] = c->text[i];
    out[n] = 0;
    return n;
}

static int valid_entry(struct ubootcfg *c, int e) {
    if (e < 0 || e >= c->count) { c->why = "no such entry"; return 0; }
    return 1;
}

static int word_ok(const char *w) {
    return strlen(w) < UBOOTCFG_WORD && plain_word(w, (int)strlen(w));
}

static int write_boot(struct ubootcfg *c, int e, const char *kernel,
                      const char *const *words, int n) {
    struct ubootcfg_entry *en = &c->entry[e];
    if (en->boot < 0) { c->why = "the entry has no multiboot2 line"; return -1; }
    if (!en->plain) { c->why = "the entry's boot line uses GRUB quoting or variables -- edit it as text"; return -1; }
    if (n > UBOOTCFG_WORDS) { c->why = "too many boot words"; return -1; }
    for (int i = 0; i < n; i++)
        if (!word_ok(words[i])) { c->why = "a boot word may not contain spaces, quotes, ; # { } or \\, nor $ except a final $name"; return -1; }
    if (!*kernel || !word_ok(kernel)) { c->why = "not a kernel path"; return -1; }

    static char line[UBOOTCFG_WORDS * UBOOTCFG_WORD + 256];
    int len = indent_of(c, en->boot, line, 64);
    const char *p = c->text + c->line_at[en->boot] + len;
    while (!is_space(*p) && *p != '\n' && *p) line[len++] = *p++;   // the command
    len += snprintf(line + len, sizeof line - (size_t)len, " %s", kernel);
    for (int i = 0; i < n; i++)
        len += snprintf(line + len, sizeof line - (size_t)len, " %s", words[i]);
    snprintf(line + len, sizeof line - (size_t)len, "\n");
    return splice(c, en->boot, 1, line);
}

int ubootcfg_set_words(struct ubootcfg *c, int e, const char *const *words, int n) {
    if (!valid_entry(c, e)) return -1;
    static char kernel[UBOOTCFG_PATH];
    snprintf(kernel, sizeof kernel, "%s", c->entry[e].kernel);
    return write_boot(c, e, kernel, words, n);
}

int ubootcfg_set_kernel(struct ubootcfg *c, int e, const char *path) {
    if (!valid_entry(c, e)) return -1;
    static struct ubootcfg_entry copy;
    const char *w[UBOOTCFG_WORDS];
    copy = c->entry[e];
    for (int i = 0; i < copy.nwords; i++) w[i] = copy.word[i];
    return write_boot(c, e, path, w, copy.nwords);
}

// "video=" for "video=1920x1080", the whole word otherwise.
static int key_len(const char *w) {
    const char *eq = strchr(w, '=');
    return eq ? (int)(eq - w) + 1 : (int)strlen(w);
}

static int same_key(const char *word, const char *key) {
    int k = key_len(key);
    if (key[k - 1] == '=') return !strncmp(word, key, (size_t)k);
    return !strcmp(word, key) || (!strncmp(word, key, (size_t)k) && word[k] == '=');
}

int ubootcfg_edit_words(struct ubootcfg *c, int e, const char *const *ops, int n) {
    if (!valid_entry(c, e)) return -1;
    static char words[UBOOTCFG_WORDS][UBOOTCFG_WORD];
    const char *ptr[UBOOTCFG_WORDS];
    int nw = c->entry[e].nwords;
    for (int i = 0; i < nw; i++) memcpy(words[i], c->entry[e].word[i], UBOOTCFG_WORD);
    for (int o = 0; o < n; o++) {
        const char *op = ops[o];
        int del = op[0] == '-', add = op[0] == '+';
        const char *w = del || add ? op + 1 : op;
        if (!word_ok(w)) { c->why = "a boot word may not contain spaces, quotes, ; # { } or \\, nor $ except a final $name"; return -1; }
        int kept = 0, found = -1;
        for (int i = 0; i < nw; i++) {
            if (same_key(words[i], w)) {
                if (del) continue;
                if (found >= 0) continue;          // one word per key
                found = kept;
            }
            if (kept != i) memcpy(words[kept], words[i], UBOOTCFG_WORD);
            kept++;
        }
        nw = kept;
        if (del) continue;
        if (found >= 0) {
            snprintf(words[found], UBOOTCFG_WORD, "%s", w);
        } else {
            if (nw >= UBOOTCFG_WORDS) { c->why = "too many boot words"; return -1; }
            snprintf(words[nw++], UBOOTCFG_WORD, "%s", w);
        }
    }
    for (int i = 0; i < nw; i++) ptr[i] = words[i];
    return ubootcfg_set_words(c, e, ptr, nw);
}

static int title_ok(struct ubootcfg *c, const char *t, int except) {
    if (!*t || strlen(t) >= UBOOTMENU_TITLE) { c->why = "a title must be 1 to 95 characters"; return 0; }
    for (const char *p = t; *p; p++)
        if ((unsigned char)*p < ' ' || strchr("\"\\$", *p)) {
            c->why = "a title may not contain \" \\ $ or control characters";
            return 0;
        }
    if (all_digits(t)) { c->why = "a title may not be a number -- it would name an entry by position"; return 0; }
    for (int i = 0; i < c->count; i++)
        if (i != except && !strcmp(c->entry[i].title, t)) { c->why = "another entry already has that title"; return 0; }
    return 1;
}

// The entry's first line with `title` in place of the old one; any
// options after the title (`--class`, the `{`) are kept.
static void retitled(const struct ubootcfg *c, int e, const char *title, char *out, int cap) {
    const struct ubootcfg_entry *en = &c->entry[e];
    char ind[64];
    indent_of(c, en->line, ind, sizeof ind);
    char rest[512];
    ubootcfg_line(c, en->line, rest, sizeof rest);
    const char *after = en->title_rest < (int)strlen(rest) ? rest + en->title_rest : "";
    snprintf(out, (size_t)cap, "%smenuentry \"%s\"%s\n", ind, title, after);
}

// An edit made of two steps runs on a scratch copy, committed whole, so
// a refused second step leaves nothing of the first.
static struct ubootcfg g_scratch;

static int commit(struct ubootcfg *c, int rc) {
    if (rc < 0) c->why = g_scratch.why;
    else memcpy(c, &g_scratch, sizeof g_scratch);
    return rc;
}

int ubootcfg_set_title(struct ubootcfg *c, int e, const char *title) {
    if (!valid_entry(c, e) || !title_ok(c, title, e)) return -1;
    char line[600];
    retitled(c, e, title, line, sizeof line);
    // A default named by title follows the rename.
    int by_title = c->defspec[0] && !all_digits(c->defspec) && c->def == e;
    memcpy(&g_scratch, c, sizeof *c);
    int rc = splice(&g_scratch, g_scratch.entry[e].line, 1, line);
    if (rc == 0 && by_title) rc = ubootcfg_set_default(&g_scratch, e);
    return commit(c, rc);
}

// Rewrite (or add, before the first entry) a top-level `set name=value`.
static int set_line(struct ubootcfg *c, int at, const char *name, const char *value) {
    char line[160];
    if (at >= 0) {
        char ind[64];
        indent_of(c, at, ind, sizeof ind);
        snprintf(line, sizeof line, "%sset %s=%s\n", ind, name, value);
        return splice(c, at, 1, line);
    }
    snprintf(line, sizeof line, "set %s=%s\n", name, value);
    return splice(c, c->count ? c->entry[0].line : c->nlines, 0, line);
}

int ubootcfg_set_default(struct ubootcfg *c, int e) {
    if (!valid_entry(c, e)) return -1;
    char v[UBOOTMENU_TITLE + 2];
    if (c->defspec[0] && !all_digits(c->defspec))
        snprintf(v, sizeof v, "\"%s\"", c->entry[e].title);
    else
        snprintf(v, sizeof v, "%d", e);
    return set_line(c, c->default_line, "default", v);
}

int ubootcfg_set_timeout(struct ubootcfg *c, int seconds) {
    if (seconds < 0 || seconds > 3600) { c->why = "a timeout is 0 to 3600 seconds"; return -1; }
    char v[16];
    snprintf(v, sizeof v, "%d", seconds);
    return set_line(c, c->timeout_line, "timeout", v);
}

// A numeric default moves with the entries; a titled one needs nothing.
static int renumber_default(struct ubootcfg *c, int was) {
    if (!all_digits(c->defspec) || was < 0 || was == c->def) return 0;
    char v[16];
    snprintf(v, sizeof v, "%d", was);
    return set_line(c, c->default_line, "default", v);
}

int ubootcfg_copy(struct ubootcfg *c, int e, const char *title) {
    if (!valid_entry(c, e)) return -1;
    if (c->entry[e].end < 0) { c->why = "the entry is never closed"; return -1; }
    if (c->count >= UBOOTMENU_MAX) { c->why = "the menu is full"; return -1; }
    if (!title_ok(c, title, -1)) return -1;
    static char block[UBOOTCFG_MAX];
    const struct ubootcfg_entry *en = &c->entry[e];
    int n = snprintf(block, sizeof block, "\n");
    retitled(c, e, title, block + n, (int)sizeof block - n);
    n += (int)strlen(block + n);
    int a = c->line_at[en->line + 1], b = c->line_at[en->end + 1];
    if (n + (b - a) + 2 >= (int)sizeof block) { c->why = "the file would be too big"; return -1; }
    memcpy(block + n, c->text + a, (size_t)(b - a));
    n += b - a;
    if (block[n - 1] != '\n') block[n++] = '\n';
    block[n] = 0;
    int def = c->def;
    if (splice(c, en->end + 1, 0, block) < 0) return -1;
    if (renumber_default(c, def > e ? def + 1 : def) < 0) return -1;
    return e + 1;
}

int ubootcfg_remove(struct ubootcfg *c, int e) {
    if (!valid_entry(c, e)) return -1;
    if (c->count <= 1) { c->why = "the last entry cannot be removed"; return -1; }
    if (e == c->def) { c->why = "that is the default entry -- make another one the default first"; return -1; }
    if (c->entry[e].end < 0) { c->why = "the entry is never closed"; return -1; }
    int first = c->entry[e].line, last = c->entry[e].end;
    // Take one blank line with it, so a copy-then-remove leaves no trace.
    char l[4];
    ubootcfg_line(c, first - 1, l, sizeof l);
    if (first > 0 && !l[0]) first--;
    int def = c->def;
    if (splice(c, first, last - first + 1, "") < 0) return -1;
    return renumber_default(c, def > e ? def - 1 : def);
}

// --- bootpart= --------------------------------------------------------------

// What grub.cfg's own entries carry (the repo's grub.cfg, kept in step):
// the PARTUUID of the partition GRUB loaded from, so the kernel roots on
// that disk. `true` because a GRUB `if` needs a body.
static const char BOOTPART_SET[]   = "set bootpart=";
static const char BOOTPART_PROBE[] = "if probe --part-uuid --set=bootpart $root; then true; fi";
#define BOOTPART_WORD "bootpart=$bootpart"

static int has_bootpart(const struct ubootcfg_entry *en) {
    for (int k = 0; k < en->nwords; k++)
        if (!strncmp(en->word[k], "bootpart=", 9)) return 1;
    return 0;
}

// An editable toy-os entry without the word. A `linux` line boots some
// other system, whose kernel has its own idea of a boot line.
static int wants_bootpart(const struct ubootcfg *c, int e) {
    const struct ubootcfg_entry *en = &c->entry[e];
    if (en->boot < 0 || !en->plain || has_bootpart(en)) return 0;
    const char *p = c->text + c->line_at[en->boot];
    const char *end = c->text + c->line_at[en->boot + 1];
    return starts_word(skip_blank(p, end), end, "multiboot2");
}

int ubootcfg_bootpart_missing(const struct ubootcfg *c) {
    int n = 0;
    for (int e = 0; e < c->count; e++) n += wants_bootpart(c, e);
    return n;
}

int ubootcfg_add_bootpart(struct ubootcfg *c) {
    int added = 0;
    for (int e = 0; e < c->count; e++) {
        if (!wants_bootpart(c, e)) continue;
        const char *op[] = { "+" BOOTPART_WORD };
        if (ubootcfg_edit_words(c, e, op, 1) < 0) return -1;
        // The probe goes ABOVE the boot line, at its indentation: the
        // word is expanded when that line runs.
        static char lines[256];
        char ind[64];
        indent_of(c, c->entry[e].boot, ind, sizeof ind);
        snprintf(lines, sizeof lines, "%s%s\n%s%s\n", ind, BOOTPART_SET, ind, BOOTPART_PROBE);
        if (splice(c, c->entry[e].boot, 0, lines) < 0) return -1;
        added++;
    }
    return added;
}

// --- trying an entry once ------------------------------------------------

int ubootcfg_trial_source(const struct ubootcfg *c, int t) {
    if (t < 0 || t >= c->count) return -1;
    const char *title = c->entry[t].title;
    size_t n = strlen(title), s = sizeof UBOOTCFG_TRIAL - 1;
    if (n <= s || strcmp(title + n - s, UBOOTCFG_TRIAL)) return -1;
    char base[UBOOTMENU_TITLE];
    memcpy(base, title, n - s);
    base[n - s] = 0;
    for (int i = 0; i < c->count; i++)
        if (!strcmp(c->entry[i].title, base)) return i;
    return -1;
}

int ubootcfg_trial_find(const struct ubootcfg *c) {
    for (int i = 0; i < c->count; i++)
        if (ubootcfg_trial_source(c, i) >= 0) return i;
    return -1;
}

int ubootcfg_try(struct ubootcfg *c, int e, const char *const *words, int n) {
    if (!valid_entry(c, e)) return -1;
    if (ubootcfg_trial_source(c, e) >= 0) { c->why = "that entry is itself a trial"; return -1; }
    char title[UBOOTMENU_TITLE];
    if (strlen(c->entry[e].title) + sizeof UBOOTCFG_TRIAL > sizeof title) { c->why = "the title is too long to add \"" UBOOTCFG_TRIAL "\" to"; return -1; }
    snprintf(title, sizeof title, "%s" UBOOTCFG_TRIAL, c->entry[e].title);
    const char *w[UBOOTCFG_WORDS];
    int nw = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(words[i], UBOOTCFG_MARK)) continue;
        if (nw >= UBOOTCFG_WORDS - 1) { c->why = "too many boot words"; return -1; }
        w[nw++] = words[i];
    }
    w[nw++] = UBOOTCFG_MARK;
    memcpy(&g_scratch, c, sizeof *c);
    int t = ubootcfg_find(&g_scratch, title);
    if (t < 0) t = ubootcfg_copy(&g_scratch, e, title);
    if (t >= 0 && ubootcfg_set_words(&g_scratch, t, w, nw) < 0) t = -1;
    return commit(c, t) < 0 ? -1 : t;
}

int ubootcfg_keep(struct ubootcfg *c, int t) {
    int s = ubootcfg_trial_source(c, t);
    if (s < 0) { c->why = "that entry is not a trial"; return -1; }
    static struct ubootcfg_entry tr;
    const char *w[UBOOTCFG_WORDS];
    tr = c->entry[t];
    int nw = 0;
    for (int i = 0; i < tr.nwords; i++)
        if (strcmp(tr.word[i], UBOOTCFG_MARK)) w[nw++] = tr.word[i];
    memcpy(&g_scratch, c, sizeof *c);
    int rc = write_boot(&g_scratch, s, tr.kernel, w, nw);
    if (rc == 0) rc = ubootcfg_remove(&g_scratch, t);
    return commit(c, rc);
}

int ubootcfg_cmdline(char *out, int cap) {
    struct query_cmdline q;
    int n = 0;
    out[0] = 0;
    for (int i = 0; sys_query_record(QUERY_CMDLINE, i, &q, sizeof q) == (int)sizeof q; i++) {
        int len = (int)strlen(q.part);
        if (n + len >= cap) return -1;
        memcpy(out + n, q.part, (size_t)len);
        n += len;
    }
    out[n] = 0;
    return n ? n : -1;
}

int ubootcfg_trial_booted(const char *cmdline) {
    size_t n = sizeof UBOOTCFG_MARK - 1;
    for (const char *p = cmdline; (p = strstr(p, UBOOTCFG_MARK)) != 0; p += n)
        if ((p == cmdline || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return 1;
    return 0;
}

// --- checking ------------------------------------------------------------

static int add(struct ubootcfg_problem *out, int max, int *n, int line, int level, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));

static int add(struct ubootcfg_problem *out, int max, int *n, int line, int level, const char *fmt, ...) {
    if (*n >= max) return 0;
    out[*n].line = line;
    out[*n].level = level;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out[*n].msg, sizeof out[*n].msg, fmt, ap);
    va_end(ap);
    (*n)++;
    return 1;
}

static void check_level(const struct ubootcfg *c, const struct ubootcfg *before, int flags,
                        int level, struct ubootcfg_problem *out, int max, int *n) {
    if (level == UBOOTCFG_BROKEN) {
        if (c->quote_line >= 0)
            add(out, max, n, c->quote_line, level, "a quote is never closed");
        if (c->open_line >= 0)
            add(out, max, n, c->open_line, level, "a { here is never closed -- missing }");
        if (c->stray_line >= 0)
            add(out, max, n, c->stray_line, level, "a } here closes nothing");
        if (c->count == 0)
            add(out, max, n, -1, level, "there is no menuentry");
        if (c->defspec[0] && c->def < 0)
            add(out, max, n, c->default_line, level, "set default=%s names no entry", c->defspec);
        if (c->timeout == -2)
            add(out, max, n, c->timeout_line, level, "set timeout= is not a number of seconds");
    }
    for (int i = 0; i < c->count; i++) {
        const struct ubootcfg_entry *e = &c->entry[i];
        if (level == UBOOTCFG_BROKEN && e->boot < 0 && e->end >= 0)
            add(out, max, n, e->line, level, "menuentry \"%s\" has no multiboot2 line", e->title);
        if (e->boot >= 0 && (flags & UBOOTCFG_CHECK_FILES) && e->kernel[0] == '/') {
            char p[UBOOTCFG_PATH + 8];
            struct stat st;
            snprintf(p, sizeof p, "/boot%s", e->kernel);
            // A missing kernel is fatal only for the entry booted unasked.
            int want = i == (c->def < 0 ? 0 : c->def) ? UBOOTCFG_BROKEN : UBOOTCFG_RISKY;
            if (want == level && stat(p, &st) < 0)
                add(out, max, n, e->boot, level, "%s does not exist on /boot", e->kernel);
        }
        if (level != UBOOTCFG_RISKY) continue;
        for (int k = 0; k < e->nwords; k++) {
            if (ubootword_find(e->word[k])) continue;
            const struct ubootword *s = ubootword_suggest(e->word[k]);
            char key[UBOOTCFG_WORD];
            snprintf(key, sizeof key, "%.*s", key_len(e->word[k]), e->word[k]);
            if (s)
                add(out, max, n, e->boot, level, "\"%s\" is not a boot word -- did you mean \"%s\"?", key, s->key);
            else
                add(out, max, n, e->boot, level, "\"%s\" is not a boot word (docs/boot-flags.md)", key);
        }
        for (int j = 0; j < i; j++)
            if (!strcmp(c->entry[j].title, e->title))
                add(out, max, n, e->line, level, "two entries are titled \"%s\" -- only the first can be chosen by name", e->title);
    }
    if (level == UBOOTCFG_RISKY) {
        if (c->timeout == 0)
            add(out, max, n, c->timeout_line, level,
                "timeout 0 hides the menu, and System Update then refuses to install a kernel");
        if (before && before->oneshot && !c->oneshot)
            add(out, max, n, -1, level, "the next_entry stanza is gone -- reboot --entry and trials stop working");
        if (c->too_many)
            add(out, max, n, -1, level, "more than %d entries -- the rest cannot be chosen here", UBOOTMENU_MAX);
    }
}

int ubootcfg_check(const struct ubootcfg *c, const struct ubootcfg *before,
                   int flags, struct ubootcfg_problem *out, int max) {
    int n = 0;
    check_level(c, before, flags, UBOOTCFG_BROKEN, out, max, &n);
    check_level(c, before, flags, UBOOTCFG_RISKY, out, max, &n);
    return n;
}

int ubootcfg_has(const struct ubootcfg_problem *p, int n, int level) {
    for (int i = 0; i < n; i++) if (p[i].level == level) return 1;
    return 0;
}

// --- saving --------------------------------------------------------------

static int under_boot(const char *path) { return !strncmp(path, "/boot/", 6); }

static int write_all(const char *path, const char *buf, int len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    long w = write(fd, buf, (size_t)len);
    int rc = w == len && fsync(fd) == 0 ? 0 : -1;
    close(fd);
    return rc;
}

static int same_as(const char *path, const char *buf, int len) {
    static char back[UBOOTCFG_MAX + 1];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    int n = 0;
    for (;;) {
        long r = read(fd, back + n, (size_t)(UBOOTCFG_MAX + 1 - n));
        if (r <= 0) break;
        n += (int)r;
        if (n > UBOOTCFG_MAX) break;
    }
    close(fd);
    return n == len && !memcmp(back, buf, (size_t)len);
}

int ubootcfg_save(const struct ubootcfg *c, const char *path, const char **why) {
    char nw[160], bak[160], dev[64] = "";
    snprintf(nw, sizeof nw, "%s.new", path);
    snprintf(bak, sizeof bak, "%s.bak", path);
    if (under_boot(path) && uboot_writable(dev, sizeof dev) < 0) {
        *why = "cannot remount /boot read-write";
        return -1;
    }
    int rc = -1;
    if (write_all(nw, c->text, c->len) < 0 || !same_as(nw, c->text, c->len)) {
        *why = "writing the new file failed";
        unlink(nw);
        goto out;
    }
    unlink(bak);
    if (sys_rename(path, bak) < 0) {
        *why = "could not keep the old file as .bak";
        unlink(nw);
        goto out;
    }
    if (sys_rename(nw, path) < 0) {
        sys_rename(bak, path);
        *why = "could not rename the new file in -- the old one is back";
        goto out;
    }
    rc = 0;
out:
    if (under_boot(path)) uboot_restore(dev);
    return rc;
}

int ubootcfg_undo(const char *path, const char **why) {
    char bak[160], swap[160], dev[64] = "";
    struct stat st;
    snprintf(bak, sizeof bak, "%s.bak", path);
    snprintf(swap, sizeof swap, "%s.swp", path);
    if (stat(bak, &st) < 0) { *why = "there is no .bak to go back to"; return -1; }
    if (under_boot(path) && uboot_writable(dev, sizeof dev) < 0) {
        *why = "cannot remount /boot read-write";
        return -1;
    }
    int rc = -1;
    unlink(swap);
    if (sys_rename(path, swap) < 0) { *why = "could not move the current file aside"; goto out; }
    if (sys_rename(bak, path) < 0) {
        sys_rename(swap, path);
        *why = "could not rename the .bak in -- nothing changed";
        goto out;
    }
    sys_rename(swap, bak);
    rc = 0;
out:
    if (under_boot(path)) uboot_restore(dev);
    return rc;
}

void ubootcfg_diff(const char *a, int alen, const char *b, int blen,
                   void (*line)(char sign, const char *s, int n, void *ctx), void *ctx) {
    int pa = 0, pb = 0;
    // Common leading lines.
    for (;;) {
        const char *ea = memchr(a + pa, '\n', (size_t)(alen - pa));
        const char *eb = memchr(b + pb, '\n', (size_t)(blen - pb));
        int la = (ea ? (int)(ea - a) + 1 : alen) - pa, lb = (eb ? (int)(eb - b) + 1 : blen) - pb;
        if (!la || la != lb || memcmp(a + pa, b + pb, (size_t)la)) break;
        pa += la;
        pb += lb;
    }
    // Common trailing lines, not reaching back past the leading ones.
    int ta = alen, tb = blen;
    while (ta > pa && tb > pb) {
        int sa = ta - 1, sb = tb - 1;
        while (sa > pa && a[sa - 1] != '\n') sa--;
        while (sb > pb && b[sb - 1] != '\n') sb--;
        if (ta - sa != tb - sb || memcmp(a + sa, b + sb, (size_t)(ta - sa))) break;
        ta = sa;
        tb = sb;
    }
    for (int p = pa; p < ta; ) {
        const char *e = memchr(a + p, '\n', (size_t)(ta - p));
        int n = (e ? (int)(e - a) : ta) - p;
        line('-', a + p, n, ctx);
        p += n + 1;
    }
    for (int p = pb; p < tb; ) {
        const char *e = memchr(b + p, '\n', (size_t)(tb - p));
        int n = (e ? (int)(e - b) : tb) - p;
        line('+', b + p, n, ctx);
        p += n + 1;
    }
}
