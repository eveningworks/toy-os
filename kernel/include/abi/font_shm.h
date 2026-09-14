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

// **TWO FAMILIES, AND THE SLOT IS family * FONT_WEIGHT_COUNT + weight.**
// The UI family is proportional and the monospace one is what a
// terminal or a code view draws with -- GNOME's `font-name` and
// `monospace-font-name`, and Windows' UI font beside Consolas. A widget
// wants the first and a grid of cells wants the second, and no single
// face can be both.
//
// Encoded into the EXISTING one-number name rather than a second field,
// so `font.0`/`font.1` still mean exactly what they meant -- the UI
// family's regular and bold -- and the mono family is added at
// `font.2`/`font.3`. A client built before this reads the same object
// it always did.
#define FONT_SHM_NAME_FMT  "font.%d"   // slot: font.0..font.3
#define FONT_SHM_MAGIC     0x464E5431u // 'FNT1'
#define FONT_SHM_VERSION   1

enum font_family {
    FONT_FAMILY_UI   = 0,  // proportional: menus, labels, titles
    FONT_FAMILY_MONO = 1,  // fixed cell: terminals, code
    FONT_FAMILY_COUNT = 2,
};

// The slot a (family, weight) pair publishes to and maps from. One
// place, because the two sides of this ABI computing it separately is
// the drift that makes a client read the wrong atlas and look merely
// ugly.
#define FONT_SHM_SLOT(family, weight) ((family) * 2 + (weight))
#define FONT_SHM_SLOT_COUNT (FONT_FAMILY_COUNT * 2)

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

    // **APPENDED, and that is why the version did not have to move.**
    // Every table above is reached through an EXPLICIT offset, so a
    // reader built before this field existed computes the same glyph
    // address from the same `glyph_off` and simply never looks here.
    // Inserting it higher up would have shifted `cell_w` onwards and
    // silently handed such a reader garbage metrics.
    uint32_t family;     // enum font_family -- see FONT_SHM_SLOT
};

// --- the beacon -------------------------------------------------------
//
// **A REPUBLISH MAKES A NEW OBJECT, SO A CLIENT'S MAPPING CANNOT TELL IT
// ANYTHING.** fontd unlinks and re-creates on a face or size change (see
// above), which means the generation a client already has mapped never
// moves -- it is reading the OLD atlas, faithfully, forever.
//
// So there is a second, tiny object that is only ever written IN PLACE.
// A client maps its one page once and reads `generation` as an ordinary
// memory access, no syscall, so checking every frame is free; when it
// moves, the client re-opens the atlas by name.
//
// The alternative was to order the change notification against fontd's
// republish, and that cannot be made reliable: the setting is applied by
// whoever changed it and fontd notices on its own poll, so the event
// arrives BEFORE the new atlas exists. A client that re-opened then
// would map the old object again and stay there. Polling a word has no
// ordering to get wrong. This is `/bin/soundd`'s beacon, same shape.
#define FONT_BEACON_NAME  "font.beacon"
#define FONT_BEACON_MAGIC 0x464E4243u // 'FNBC'

struct font_beacon {
    uint32_t magic;      // FONT_BEACON_MAGIC
    uint32_t generation; // bumped AFTER every weight has been republished
};

#endif
