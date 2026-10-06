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

// --- the inline rules, for a SECOND renderer --------------------------
//
// `code`, **bold**, [text](link), the one backslash escape and the
// transliteration of anything above ASCII. Walked here once and
// reported as styled characters, because there are two renderers now --
// this file's text one, and the GUI widget in ui/uui_markdown.c -- and
// two implementations would be two subtly different ideas of what `**`
// means in a document nobody would think to check both ways.
//
// Whitespace is reported as-is rather than swallowed: a caller wrapping
// text needs to see where a word ends, and a caller drawing it needs
// the space itself.
#define UMD_STYLE_BOLD 1
#define UMD_STYLE_CODE 2

typedef void (*umd_inline_fn)(void *ctx, char c, unsigned style);

void umd_inline_walk(const char *s, int n, umd_inline_fn emit, void *ctx);

// --- line classification, for the same reason -------------------------
//
// What KIND of line this is. A block renderer needs exactly these
// questions answered, and answering them a second time is how two
// renderers disagree about whether `    x` is a code block.
enum umd_block {
    UMD_BLANK,
    UMD_HEADING,   // `arg` is the level, 1..6
    UMD_BULLET,    // `arg` is the bytes of marker to skip
    UMD_NUMBERED,  // likewise
    UMD_FENCE,     // ``` or ~~~ -- toggles a verbatim run
    UMD_PRE,       // an indented (four-space) code line
    UMD_TABLE,     // a `|`-delimited row; `arg` is 1 for the ---|--- rule
    UMD_RULE,      // a horizontal rule
    // A FOLD, GitHub's <details> shape, each tag ON ITS OWN LINE:
    //     <details>            (`<details open>`: `arg` 1, starts open)
    //     <summary>Text</summary>
    //     ...the folded Markdown...
    //     </details>
    // A renderer that can fold draws the summary as a row that opens it;
    // one that cannot (a terminal) shows the summary and everything in it.
    UMD_DETAILS,
    UMD_SUMMARY,   // `text` is what is between the two tags
    UMD_DETAILS_END,
    UMD_PARA,      // anything else
};

// `*text`/`*len` come back as the line's CONTENT with the marker
// removed, so a caller draws what it is given.
enum umd_block umd_classify(const char *line, int n, int *arg,
                            const char **text, int *text_len);

// One line of `src`, without its newline. `*i` advances past it.
// Returns the line's length; `*out` points into `src`.
int umd_next_line(const char *src, int len, int *i, const char **out);

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
