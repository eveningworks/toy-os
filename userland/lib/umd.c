// The Markdown renderer. See umd.h for the subset and why it is that
// subset; this file is the machinery.
//
// IT IS ONE PASS AND ALLOCATES NOTHING. Lines are classified as they
// arrive and fed to an emitter that wraps as it goes, so the only
// buffer is one WORD -- a paragraph is never held whole, and a page of
// any size renders inside a 2 KiB ring-3 stack frame. The escapes a
// style change emits travel INSIDE the word buffer rather than going
// straight out, which is what keeps a style opened just before a wrap
// from landing on the line above it.
//
// **A TABLE IS RENDERED AS ONE BLOCK PER ROW, not as aligned columns**,
// and that is a judgement rather than a shortcut. Aligning needs the
// whole table buffered to measure it, and these tables do not fit
// anyway -- `config.md` has a 250-character cell against an 80-column
// console, so alignment would produce two columns of one word each.
// `Header: value` with a hanging indent reads correctly at every width
// and needs nothing buffered but the header row.
#include "lib/umd.h"
#include <string.h>

// ---------------------------------------------------------------- sink

static void ob(struct umd_out *o, char c) {
    if (o->len >= o->cap) { o->overflow = 1; return; }
    o->buf[o->len++] = c;
}

static void os_(struct umd_out *o, const char *s) {
    while (*s) ob(o, *s++);
}

// ------------------------------------------------------ transliteration

// The pages are written with typographic punctuation and the console
// draws a small ASCII glyph set, so a raw em dash arrives as three
// boxes. Everything else above 0x7f becomes '?': a wrong character is
// one character, and a half-decoded sequence is three.
static const struct { const char *from, *to; } TRANSLIT[] = {
    { "\xe2\x80\x94", "--" },    // em dash
    { "\xe2\x80\x93", "-" },     // en dash
    { "\xe2\x86\x92", "->" },    // rightwards arrow
    { "\xe2\x88\x92", "-" },     // minus sign
    { "\xe2\x80\xa6", "..." },   // ellipsis
    { "\xe2\x80\x98", "'" },     // left single quote
    { "\xe2\x80\x99", "'" },     // right single quote
    { "\xe2\x80\x9c", "\"" },    // left double quote
    { "\xe2\x80\x9d", "\"" },    // right double quote
    { "\xc2\xa7",     "S" },     // section sign
    { "\xc2\xb1",     "+/-" },   // plus-minus
    { "\xc2\xa0",     " " },     // no-break space
};

// Bytes in the UTF-8 sequence starting at `s`, at least 1.
static int utf8_len(const char *s, int n) {
    unsigned char c = (unsigned char)s[0];
    int want = (c < 0xc0) ? 1 : (c < 0xe0) ? 2 : (c < 0xf0) ? 3 : 4;
    if (want > n) want = n;
    return want;
}

// --------------------------------------------------------- the emitter

#define WORD_CAP 192

struct emit {
    struct umd_out *out;
    int cols;
    int color;
    int left;        // indent of a block's first line
    int hang;        // indent of its continuation lines
    int col;         // display columns used on the current output line
    int open;        // a line has been started and not yet ended
    int started;     // at least one line of this block has been emitted
    int blank;       // a blank line is owed before the next block
    int bold, code;  // active inline styles
    int upper;       // fold to upper case, for a section heading
    char w[WORD_CAP];
    int  wn, ww;     // pending word: bytes, and display columns
};

static void e_nl(struct emit *e) {
    ob(e->out, '\n');
    e->col = 0;
    e->open = 0;
}

// Bytes that occupy no display columns -- an SGR sequence. They ride in
// the word buffer so a style cannot be separated from the text it
// styles by a line break.
static void e_esc(struct emit *e, const char *s) {
    while (*s && e->wn < WORD_CAP) e->w[e->wn++] = *s++;
}

static void e_word(struct emit *e, int continued) {
    if (e->wn == 0) return;
    if (!e->open) {
        int ind = e->started ? e->hang : e->left;
        for (int i = 0; i < ind; i++) ob(e->out, ' ');
        e->col = ind;
        e->open = 1;
        e->started = 1;
    } else if (!continued && e->col + 1 + e->ww > e->cols) {
        e_nl(e);
        for (int i = 0; i < e->hang; i++) ob(e->out, ' ');
        e->col = e->hang;
        e->open = 1;
    } else if (!continued) {
        ob(e->out, ' ');
        e->col++;
    }
    for (int i = 0; i < e->wn; i++) ob(e->out, e->w[i]);
    e->col += e->ww;
    e->wn = e->ww = 0;
}

// One visible character into the pending word. An overlong token (a URL,
// a long code span) is flushed as a CONTINUATION rather than dropped, so
// it runs on instead of losing its tail.
static void e_ch(struct emit *e, char c) {
    if (e->upper && c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (e->wn + 8 > WORD_CAP) e_word(e, 1);
    e->w[e->wn++] = c;
    e->ww++;
}

static void e_str(struct emit *e, const char *s) {
    while (*s) e_ch(e, *s++);
}

// Re-specifies the whole style rather than closing one attribute, so
// bold and code can nest in either order without a close undoing the
// other. ESC[22m would be the narrower reset; a full re-specification
// cannot get the pairing wrong.
static void e_style(struct emit *e) {
    if (!e->color) return;
    if (e->bold && e->code)  e_esc(e, "\x1b[1;36m");
    else if (e->bold)        e_esc(e, "\x1b[1m");
    else if (e->code)        e_esc(e, "\x1b[36m");
    else                     e_esc(e, "\x1b[0m");
}

static void e_block(struct emit *e, int left, int hang) {
    if (e->open) { e_word(e, 0); e_nl(e); }
    if (e->blank && e->out->len) { ob(e->out, '\n'); e->blank = 0; }
    e->left = left;
    e->hang = hang;
    e->started = 0;
}

static void e_end(struct emit *e) {
    e_word(e, 0);
    if (e->open) e_nl(e);
    e->blank = 1;
}

// Opens a line explicitly, for a list marker that must sit before the
// first word. The word that follows supplies its own separating space.
static void e_marker(struct emit *e, int ind, const char *marker) {
    for (int i = 0; i < ind; i++) ob(e->out, ' ');
    e->col = ind;
    e->open = 1;
    e->started = 1;
    if (e->color) os_(e->out, "\x1b[1m");
    for (const char *m = marker; *m; m++) { ob(e->out, *m); e->col++; }
    if (e->color) os_(e->out, "\x1b[0m");
}

// --------------------------------------------------------- inline text

// Feeds `n` bytes of Markdown through the emitter, honouring `code`,
// **bold** and [text](link) and transliterating anything above ASCII.
// Whitespace ends a word; a newline is whitespace, which is what lets a
// paragraph be fed one source line at a time with no buffer.
static void inline_text(struct emit *e, const char *s, int n) {
    for (int i = 0; i < n; ) {
        char c = s[i];

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            e_word(e, 0);
            i++;
            continue;
        }
        // The ONE backslash escape implemented, and only because a table
        // needs it: everything else written `\x` in these pages is
        // inside a code span, where a backslash is the text.
        if (c == '\\' && i + 1 < n && s[i + 1] == '|') {
            e_ch(e, '|');
            i += 2;
            continue;
        }
        if (c == '`') {
            e->code = !e->code;
            e_style(e);
            i++;
            continue;
        }
        if (!e->code && c == '*' && i + 1 < n && s[i + 1] == '*') {
            e->bold = !e->bold;
            e_style(e);
            i += 2;
            continue;
        }
        // [text](url) -- the text is kept, the url dropped. Every link
        // in these pages points at a sibling page, which is a path on
        // the host and nothing a reader can follow from in here.
        if (!e->code && c == '[') {
            int j = i + 1;
            while (j < n && s[j] != ']' && s[j] != '\n') j++;
            if (j < n && s[j] == ']' && j + 1 < n && s[j + 1] == '(') {
                int k = j + 2;
                while (k < n && s[k] != ')' && s[k] != '\n') k++;
                if (k < n && s[k] == ')') {
                    inline_text(e, s + i + 1, j - i - 1);
                    i = k + 1;
                    continue;
                }
            }
        }
        if ((unsigned char)c < 0x80) {
            e_ch(e, c);
            i++;
            continue;
        }

        int L = utf8_len(s + i, n - i);
        const char *rep = "?";
        for (size_t t = 0; t < sizeof TRANSLIT / sizeof TRANSLIT[0]; t++) {
            size_t fl = strlen(TRANSLIT[t].from);
            if ((int)fl == L && memcmp(s + i, TRANSLIT[t].from, fl) == 0) {
                rep = TRANSLIT[t].to;
                break;
            }
        }
        e_str(e, rep);
        i += L;
    }
}

// One whole line of a paragraph or list item. **THE LINE BREAK IS
// WHITESPACE AND HAS TO BE FED AS SUCH**: the caller passes a line
// WITHOUT its newline, so without this flush the last word of one line
// and the first of the next were joined into one -- which read as a
// missing space in the middle of a sentence and only showed up where a
// page happened to break a line between two code spans.
//
// Not folded into inline_text() because that function also renders the
// TEXT OF A LINK, where flushing would split a word in half.
static void inline_line(struct emit *e, const char *s, int n) {
    inline_text(e, s, n);
    e_word(e, 0);
}

// Verbatim, for a code block: no wrapping and no markup, but still
// transliterated -- a stray em dash in a code block is as unreadable as
// one in a sentence.
static void verbatim(struct emit *e, const char *s, int n, int ind) {
    for (int i = 0; i < ind; i++) ob(e->out, ' ');
    if (e->color) os_(e->out, "\x1b[36m");
    for (int i = 0; i < n; ) {
        if ((unsigned char)s[i] < 0x80) {
            if (s[i] != '\r') ob(e->out, s[i]);
            i++;
            continue;
        }
        int L = utf8_len(s + i, n - i);
        const char *rep = "?";
        for (size_t t = 0; t < sizeof TRANSLIT / sizeof TRANSLIT[0]; t++) {
            size_t fl = strlen(TRANSLIT[t].from);
            if ((int)fl == L && memcmp(s + i, TRANSLIT[t].from, fl) == 0) {
                rep = TRANSLIT[t].to;
                break;
            }
        }
        os_(e->out, rep);
        i += L;
    }
    if (e->color) os_(e->out, "\x1b[0m");
    ob(e->out, '\n');
    e->started = 1;
}

// ---------------------------------------------------- line classifying

struct line { const char *s; int n; };

static struct line next_line(const char *src, int len, int *i) {
    struct line l = { src + *i, 0 };
    int j = *i;
    while (j < len && src[j] != '\n') j++;
    l.n = j - *i;
    if (l.n && l.s[l.n - 1] == '\r') l.n--;
    *i = (j < len) ? j + 1 : len;
    return l;
}

static int blank_line(struct line l) {
    for (int i = 0; i < l.n; i++)
        if (l.s[i] != ' ' && l.s[i] != '\t') return 0;
    return 1;
}

static int starts(struct line l, const char *p) {
    int n = (int)strlen(p);
    return l.n >= n && memcmp(l.s, p, (size_t)n) == 0;
}

// `- ` at column 0. Nested bullets do not occur in these pages and are
// not recognised; an indented `- ` is code, which is what four spaces
// already means.
static int bullet(struct line l) { return starts(l, "- ") || starts(l, "* "); }

// `12. ` at column 0. Recognised only at the start of a block or after
// another item, because a WRAPPED prose line can begin with a number and
// a full stop -- one page's does, and treating it as a list item split
// the paragraph in half.
static int numbered(struct line l, int *marker_len) {
    int i = 0;
    while (i < l.n && l.s[i] >= '0' && l.s[i] <= '9') i++;
    if (i == 0 || i > 3 || i + 1 >= l.n) return 0;
    if (l.s[i] != '.' || l.s[i + 1] != ' ') return 0;
    *marker_len = i + 1;
    return 1;
}

static int table_row(struct line l) { return l.n > 0 && l.s[0] == '|'; }

// `|---|:--:|` -- the row that says a table is a table, and carries
// nothing to show.
//
// **IT MUST CONTAIN A DASH.** Without that clause `| | |` -- a table
// with deliberately empty headers, which `dmesg.md` has -- is made of
// nothing but the allowed characters and was swallowed as a rule. The
// first DATA row then became the header, so every later row was
// labelled with the previous row's text.
static int table_rule(struct line l) {
    if (!table_row(l)) return 0;
    int dash = 0;
    for (int i = 0; i < l.n; i++) {
        if (!strchr("|-: \t", l.s[i])) return 0;
        if (l.s[i] == '-') dash = 1;
    }
    return dash;
}

static int heading_level(struct line l) {
    int n = 0;
    while (n < l.n && l.s[n] == '#') n++;
    if (n == 0 || n > 6 || n >= l.n || l.s[n] != ' ') return 0;
    return n;
}

// One cell of a `| a | b |` row: its bounds, and where the next starts.
static int table_cell(struct line l, int *pos, const char **cs, int *cn) {
    int i = *pos;
    if (i >= l.n) return 0;
    if (l.s[i] == '|') i++;
    int start = i;
    // `\|` is a LITERAL pipe, not this cell's end. Without that clause
    // `| `config show <name\|path>` |` splits inside its own code span
    // and the row comes out as three cells, two of them nonsense.
    while (i < l.n && !(l.s[i] == '|' && (i == 0 || l.s[i - 1] != '\\'))) i++;
    int end = i;
    while (start < end && (l.s[start] == ' ' || l.s[start] == '\t')) start++;
    while (end > start && (l.s[end - 1] == ' ' || l.s[end - 1] == '\t')) end--;
    *pos = i;
    *cs = l.s + start;
    *cn = end - start;
    return (start < l.n) || (end > start);
}

#define TABLE_MAX_COLS 8
#define TABLE_HDR_CAP  40

// ------------------------------------------------------------- render

void umd_render(const char *src, int len,
                const struct umd_opts *o, struct umd_out *out) {
    struct emit e;
    memset(&e, 0, sizeof e);
    e.out = out;
    e.cols = o->cols > 16 ? o->cols : 80;
    e.color = o->color;

    int ind = o->indent;
    int fence = 0;
    int in_list = 0;
    // Consecutive four-space lines are ONE code block, so the block is
    // opened once. Without this each line re-opened it and the blank
    // line a block start owes was emitted between every pair of them.
    int in_pre = 0;
    char hdr[TABLE_MAX_COLS][TABLE_HDR_CAP];
    int  hdrs = 0;

    int i = 0;
    while (i < len) {
        struct line l = next_line(src, len, &i);

        if (starts(l, "```") || starts(l, "~~~")) {
            if (!fence) { e_block(&e, ind + 3, ind + 3); e.blank = 0; }
            else e.blank = 1;
            fence = !fence;
            in_pre = 0;
            continue;
        }
        if (fence) { verbatim(&e, l.s, l.n, ind + 3); continue; }

        if (blank_line(l)) {
            if (e.open) { e_word(&e, 0); e_nl(&e); }
            e.blank = 1;
            in_list = 0;
            in_pre = 0;
            hdrs = 0;
            continue;
        }

        int lv = heading_level(l);
        if (lv) {
            // Uppercased at the margin, as a man page's section headers
            // are -- it is the one convention that makes a rendered page
            // recognisable as documentation at a glance. H3 keeps its
            // case and moves in, because it is a subheading rather than
            // a section.
            e_block(&e, lv >= 3 ? ind : 0, lv >= 3 ? ind : 0);
            if (e.color) os_(out, "\x1b[1m");
            e.upper = (lv < 3);
            inline_line(&e, l.s + lv + 1, l.n - lv - 1);
            e.upper = 0;
            // The attribute spans any wrap this heading needed, and the
            // reset goes before the last newline rather than after it --
            // a bold that outlives its line bolds the blank one under it.
            if (e.color) os_(out, "\x1b[0m");
            if (e.open) e_nl(&e);
            e.blank = 1;
            in_list = 0;
            in_pre = 0;
            hdrs = 0;
            continue;
        }

        if (table_row(l)) {
            in_pre = 0;
            if (table_rule(l)) continue;
            int pos = 0, ci = 0;
            const char *cs;
            int cn;
            if (hdrs == 0) {
                // The first row of a table is its headers; they become
                // the labels every later row is rendered against.
                while (ci < TABLE_MAX_COLS && table_cell(l, &pos, &cs, &cn)) {
                    int k = 0;
                    for (; k < cn && k < TABLE_HDR_CAP - 1; k++) hdr[ci][k] = cs[k];
                    hdr[ci][k] = 0;
                    ci++;
                    if (pos >= l.n) break;
                }
                hdrs = ci ? ci : 1;
                continue;
            }
            while (ci < TABLE_MAX_COLS && table_cell(l, &pos, &cs, &cn)) {
                if (cn > 0) {
                    const char *label = (ci < hdrs) ? hdr[ci] : "";
                    int llen = (int)strlen(label);
                    int base = ind + 3;
                    // A LABEL THAT WOULD LEAVE LESS THAN HALF THE WIDTH
                    // FOR ITS VALUE GOES ON ITS OWN LINE. Hanging the
                    // value off a 40-column label in an 80-column
                    // terminal does not indent it, it wraps it one word
                    // per line -- which is what a table of prose against
                    // a narrow console actually did.
                    int inl = llen && base + llen + 2 <= e.cols / 2;
                    if (llen) {
                        e_block(&e, base, inl ? base + llen + 2 : base + 2);
                        e.blank = 0;
                        e_marker(&e, base, label);
                        e_ch(&e, ':');
                        e_word(&e, 1);
                        if (!inl) { e_nl(&e); e.started = 1; }
                    } else {
                        e_block(&e, base, base);
                        e.blank = 0;
                    }
                    inline_text(&e, cs, cn);
                    e_word(&e, 0);
                    if (e.open) e_nl(&e);
                }
                ci++;
                if (pos >= l.n) break;
            }
            e.blank = 1;
            continue;
        }

        int mlen = 0;
        if (bullet(l) || ((in_list || !e.open) && numbered(l, &mlen))) {
            char marker[6];
            int mn;
            if (mlen) {
                for (mn = 0; mn < mlen; mn++) marker[mn] = l.s[mn];
            } else {
                marker[0] = '-';
                mn = 1;
                mlen = 1;
            }
            marker[mn] = 0;
            e_block(&e, ind + 2, ind + 2 + mn + 1);
            e.blank = 0;
            e_marker(&e, ind + 2, marker);
            inline_line(&e, l.s + mlen + 1, l.n - mlen - 1);
            in_list = 1;
            in_pre = 0;
            continue;
        }

        // Four spaces at the margin is a code block -- these pages'
        // Synopsis sections are written that way, and check_docs.py
        // requires it, so this is the form that matters most.
        if (starts(l, "    ") && !in_list) {
            if (!in_pre) { e_block(&e, ind + 3, ind + 3); in_pre = 1; }
            verbatim(&e, l.s + 4, l.n - 4, ind + 3);
            e.blank = 1;
            continue;
        }

        in_pre = 0;
        if (!e.open && !in_list) e_block(&e, ind, ind);
        inline_line(&e, l.s, l.n);
    }
    e_end(&e);
}

// ------------------------------------------------------- field queries

// The renderer with colour off and no wrapping IS markup-stripped text,
// which is the point of running the lookups below through it: one idea
// of what a backtick means, not two.
int umd_plain(const char *s, int n, char *out, int cap) {
    struct umd_out o = { out, cap - 1, 0, 0 };
    struct emit e;
    memset(&e, 0, sizeof e);
    e.out = &o;
    e.cols = 1 << 24;
    inline_text(&e, s, n);
    e_word(&e, 0);
    out[o.len] = 0;
    return o.len;
}

int umd_title(const char *src, int len, char *out, int cap) {
    int i = 0;
    while (i < len) {
        struct line l = next_line(src, len, &i);
        if (heading_level(l) == 1) return umd_plain(l.s + 2, l.n - 2, out, cap);
    }
    out[0] = 0;
    return 0;
}

int umd_field(const char *src, int len, const char *name, char *out, int cap) {
    char want[64];
    int wn = 0;
    want[wn++] = '*';
    want[wn++] = '*';
    for (const char *p = name; *p && wn < (int)sizeof want - 4; p++) want[wn++] = *p;
    want[wn++] = ':';
    want[wn++] = '*';
    want[wn++] = '*';
    want[wn] = 0;

    int i = 0;
    while (i < len) {
        struct line l = next_line(src, len, &i);
        if (starts(l, want)) return umd_plain(l.s + wn, l.n - wn, out, cap);
    }
    out[0] = 0;
    return 0;
}

int umd_section_para(const char *src, int len, const char *name,
                     char *out, int cap) {
    int i = 0;
    int in = 0;
    while (i < len) {
        struct line l = next_line(src, len, &i);
        int lv = heading_level(l);
        if (lv) {
            if (in) break;                       // the section ended empty
            char h[64];
            umd_plain(l.s + lv + 1, l.n - lv - 1, h, sizeof h);
            in = (lv == 2 && strcmp(h, name) == 0);
            continue;
        }
        if (!in || blank_line(l)) continue;

        // The first paragraph, which runs until the next blank line.
        struct umd_out o = { out, cap - 1, 0, 0 };
        struct emit e;
        memset(&e, 0, sizeof e);
        e.out = &o;
        e.cols = 1 << 24;
        for (;;) {
            // inline_LINE: the newline between two source lines is
            // whitespace, and feeding the lines without it joins the
            // last word of one to the first of the next. The renderer
            // has the same trap and the same fix; this copy was written
            // before that one existed and inherited the bug.
            inline_line(&e, l.s, l.n);
            if (i >= len) break;
            int save = i;
            l = next_line(src, len, &i);
            if (blank_line(l) || heading_level(l) || table_row(l)) { i = save; break; }
        }
        e_word(&e, 0);
        out[o.len] = 0;
        return o.len;
    }
    out[0] = 0;
    return 0;
}
