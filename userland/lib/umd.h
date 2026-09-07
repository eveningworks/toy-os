#ifndef ULIB_UMD_H
#define ULIB_UMD_H

// umd -- rendering Markdown as text for a terminal.
//
// It exists because /bin/doc reads the repo's own `docs/commands/*.md`
// pages OFF THE DISK, unconverted. The alternative was a build step
// turning each page into some pre-wrapped format, and it was rejected
// for one reason: the wrap column would be frozen at build time, while
// the width a page is actually read at is whatever `sys_tcgetwinsz()`
// says a moment before it is drawn -- 80 on the console, something else
// in every Terminal window. Rendering at display time is also what man
// does with roff, and what mdcat and glow do with Markdown; a
// pre-rendered cat page is the thing man keeps only as a cache.
//
// **THE SUBSET IS WHAT THE PAGES ACTUALLY USE, and that was measured
// rather than guessed.** Headings, paragraphs, indented and fenced code,
// bullet and numbered lists, tables, and inline `code`, **bold** and
// [links](x). Not implemented, because no page contains one:
// blockquotes, horizontal rules, nested lists, images.
//
// **BACKSLASH ESCAPES ARE NOT IMPLEMENTED EITHER, WITH ONE EXCEPTION.**
// There are thirteen backslashes in 108 pages; eight are `\ `, which is
// not a valid escape and renders as itself here as it does on GitHub,
// and the rest sit inside code spans where a backslash IS the text. The
// exception is `\|`, which a table cell needs: it is how a pipe is
// written inside one, and without it the cell splits at its own text.
//
// **`_x_` IS NOT ITALIC HERE, DELIBERATELY.** 65 pages contain an
// underscore pair and every one of them is an identifier -- `SYS_NET_*`,
// `_MONOTONIC_`. Treating those as emphasis would eat the underscores
// out of the names the page exists to document. Single `*` is left
// alone for the same reason.
//
// Output is ASCII: the console and the shipped faces draw a small glyph
// set, so the em dashes and arrows the pages are written with are
// TRANSLITERATED (`--`, `->`) rather than emitted as UTF-8 that would
// arrive as three boxes each.
#include <stddef.h>

struct umd_opts {
    int cols;      // wrap to this many display columns
    int color;     // emit SGR for headings, bold and code
    int indent;    // left margin for body text under a heading
};

// A growing byte sink the CALLER owns. `overflow` is set once `buf` is
// full and further output is dropped -- reported rather than silent,
// because a page that stops mid-sentence reads as a broken page.
struct umd_out {
    char *buf;
    int   cap;
    int   len;
    int   overflow;
};

void umd_render(const char *src, int len,
                const struct umd_opts *o, struct umd_out *out);

// Markdown with its inline markup removed and its whitespace collapsed
// to single spaces: `code` and **bold** lose their markers, a link
// keeps its text, and anything above ASCII is transliterated. Returns
// the length. This is what the three lookups below are built on, and
// what a caller printing a matching LINE wants -- a search result full
// of backticks and asterisks is the raw file, not an answer.
int umd_plain(const char *src, int len, char *out, int cap);

// The text of the document's first `# ` heading, inline markup
// stripped. Returns its length, or 0 if there is none.
int umd_title(const char *src, int len, char *out, int cap);

// The value of a `**<name>:**` line -- how these pages carry their
// Category. Returns its length, or 0 if there is no such line.
int umd_field(const char *src, int len, const char *name, char *out, int cap);

// The first paragraph of the `## <name>` section, inline markup
// stripped and whitespace collapsed to single spaces. Returns its
// length, or 0 if there is no such section.
//
// A named section rather than "the first paragraph": these pages open
// with a couple of `**field:**` lines, so the first prose in the file
// is never the sentence a reader wants. WHICH section that is belongs
// to the caller -- that convention is doc's, not Markdown's.
int umd_section_para(const char *src, int len, const char *name,
                     char *out, int cap);

#endif // ULIB_UMD_H
