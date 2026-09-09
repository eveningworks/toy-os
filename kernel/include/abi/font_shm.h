#ifndef FONT_SHM_H
#define FONT_SHM_H

#include <stdint.h>

// THE SESSION FONT, PUBLISHED BY A RING-3 SERVICE.
//
// `/bin/fontd` parses the selected `.ttf` and rasterizes the atlas, and
// every client maps the result read-only out of shared memory. The
// kernel is not in this path at all: it draws its console from the baked
// tables compiled into the image and never parses a font.
//
// **WHY A SERVICE RATHER THAN A COPY PER CLIENT.** Two reasons, and the
// second is the one that bites. An atlas is tens to hundreds of KB, so a
// copy per client is large -- and, worse, free to DRIFT: a client
// carrying its own would go on rendering the old face after `fontface`
// changed it. One mapping keeps every client's text identical to the
// desktop's by construction.
//
// **WHY IT IS PUBLIC.** Every GUI client needs to read it and there is
// no list of them, so granting per process is not workable. An atlas is
// glyph bitmaps -- there is nothing in it to protect.
//
// **THE GENERATION IS HOW A CLIENT NOTICES A CHANGE.** fontd re-creates
// the object when the face or size moves; a client that mapped the old
// one keeps a valid mapping of memory nobody updates (shm keeps an
// object alive for whoever still holds it), so polling `generation` is
// what tells it to re-open. Same shape as a window buffer's generation
// -- see abi/win_proto.h.

#define FONT_SHM_NAME_FMT  "font.%d"   // ...by weight: font.0, font.1
#define FONT_SHM_MAGIC     0x464E5431u // 'FNT1'
#define FONT_SHM_VERSION   1

// A reader must tolerate a header it does not understand: fontd may be
// newer than the client. Check magic AND version, and fall back rather
// than reading fields that may have moved.
struct font_shm {
    uint32_t magic;      // FONT_SHM_MAGIC once the contents are valid
    uint32_t version;    // FONT_SHM_VERSION
    uint32_t generation; // bumped on every republish; 0 while being built

    uint32_t px;         // em size this was rasterized at
    uint32_t weight;     // enum font_weight
    uint32_t cell_w;     // the fixed cell: the widest advance in the set
    uint32_t cell_h;     // rows per glyph bitmap -- the INDEXING stride
    uint32_t line_h;     // rows between lines -- what LAYOUT uses
    uint32_t baseline;   // rows from the cell top to the baseline
    uint32_t count;      // glyphs, == FONT_TTF_GLYPH_COUNT
    uint32_t monospace;  // 1 when every advance is equal
    uint32_t synthetic;  // 1 when bold was SMEARED rather than loaded

    // Byte offsets from the START OF THIS STRUCT. Given rather than
    // derived because the header's own size is not something a client
    // should have to know, and a padded struct would shift all three.
    uint32_t glyph_off;
    uint32_t adv_off;
    uint32_t kern_off;
    uint32_t bytes;      // total object size, header included

    // The face's filename without the extension, for `fontd` diagnostics
    // and for a client that wants to say what it is drawing with.
    char face[32];
};

#endif
