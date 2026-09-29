#ifndef UUI_MARKDOWN_H
#define UUI_MARKDOWN_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_scrollbar.h"

// uui_markdown -- a Markdown DOCUMENT, drawn as a document: proportional
// text, headings at real sizes, code in a monospace face on a tinted
// ground, lists with hanging indents, rules, and tables.
//
// **THE PARSING IS NOT HERE.** `lib/umd.h` owns what Markdown means --
// `umd_classify()` for blocks and `umd_inline_walk()` for `code`,
// **bold** and links -- and this widget owns only what it LOOKS like.
// That split is the whole point: `/bin/doc` renders the same documents
// to a terminal through the other half of umd, and a second parser here
// would be two subtly different ideas of what `**` means in pages
// nobody would think to check both ways.
//
// **IT SCROLLS IN PIXELS, NOT LINES**, because a heading, a rule and a
// code block are not the same height -- which is exactly the difference
// between this and `utext`, where every row is one character cell.
//
// **THE CALLER OWNS THE TEXT.** The widget keeps a pointer and re-walks
// it every frame; it copies nothing and allocates nothing except the
// font faces below.

// The three faces this loads for itself, beyond the session's own:
// two heading sizes and a monospace for code. A load that FAILS is not
// an error -- the widget falls back to the session bold face, so a
// machine with no fonts on disk still renders a readable document.
#define UUI_MD_FACES 3
#define UUI_MD_LINKS 96       // links recorded per frame; more draw, unclickable
#define UUI_MD_LINK_MAX 48    // a target's longest name

struct uui_markdown_face {
    struct ugfx_font font;
    void *arena;
    int ok;
};

struct uui_markdown {
    // Content-relative geometry of the WHOLE control, text plus
    // scrollbar -- the same convention as every other uui_ widget.
    int x, y, w, h;

    const char *src;   // the CALLER's; not copied
    int len;

    int scroll;        // pixels from the top of the document
    int doc_h;         // its full height, from the last layout pass

    // What `doc_h` was measured against. The height only changes when
    // the text or the wrap width does, and measuring is a whole walk of
    // the document -- so remembering these turns two walks per frame
    // into one for every frame that changed neither.
    const char *m_src;
    int m_len;
    int m_w;
    int bar_w;
    int thumb_grab;    // -1 when no drag is in progress

    struct uui_markdown_face face[UUI_MD_FACES];
    int faces_tried;

    // --- links (uui_markdown_set_links) -------------------------------
    //
    // A LINK IS AN INLINE-CODE WORD THE APP SAYS NAMES SOMETHING: the
    // pages write `ping` for a command, never [ping](ping.md), so the app
    // supplies the predicate and the widget draws the word in the accent,
    // underlined. Where each was drawn is recorded as it is drawn, in
    // widget coordinates, and a press-and-release on one is reported.
    int (*is_link)(void *ctx, const char *word);
    void *link_ctx;
    struct uui_md_link { int x, y, w, h; char target[UUI_MD_LINK_MAX]; } link[UUI_MD_LINKS];
    int link_n;
    int armed_link, hover_link;   // indices into `link`, or -1
    char taken[UUI_MD_LINK_MAX];  // the last clicked target, until taken
};

void uui_markdown_init(struct uui_markdown *m);

// The document. Safe to call repeatedly; it resets the scroll only when
// the text actually changed, so a redraw does not jump to the top.
void uui_markdown_set_text(struct uui_markdown *m, const char *src, int len);

void uui_markdown_set_geometry(struct uui_markdown *m, int x, int y, int w, int h);

// **Both 0 -- no preference in either direction.** A document reflows to
// whatever width it is given and scrolls at whatever height, so any
// number here would be invented (ui/uui_primitives.h).
void uui_markdown_natural_size(const struct uui_markdown *m, int *out_w, int *out_h);

void uui_markdown_draw(struct ugfx_surface *s, struct uui_markdown *m);

// --- input; each returns 1 if it consumed the event ------------------

int  uui_markdown_wheel(struct uui_markdown *m, int notches);
int  uui_markdown_press(struct uui_markdown *m, int cx, int cy);
void uui_markdown_motion(struct uui_markdown *m, int cx, int cy);
void uui_markdown_release(struct uui_markdown *m);
int  uui_markdown_hit(const struct uui_markdown *m, int cx, int cy);

// Frees the faces it loaded. **uui_image is no longer the only widget
// that owns memory** -- this one owns three font arenas, and an app that
// drops a markdown view without calling this leaks them.
void uui_markdown_free(struct uui_markdown *m);

// Makes inline-code words for which `is_link(ctx, word)` returns 1 into
// links. NULL turns links off (the default).
void uui_markdown_set_links(struct uui_markdown *m,
                            int (*is_link)(void *ctx, const char *word), void *ctx);
// The link clicked since the last call, copied to `out`: 1, or 0 when
// none was. A click is a press and a release on the same link.
int uui_markdown_take_link(struct uui_markdown *m, char *out, int cap);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_markdown_ops;

#endif
