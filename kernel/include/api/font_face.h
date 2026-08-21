#ifndef FONT_FACE_H
#define FONT_FACE_H

// Fonts loaded from disk at runtime: the registry over /usr/share/fonts.
//
// This is the piece that makes a .ttf on the filesystem into something
// gfx.c can draw with. It scans the directory, loads the SELECTED face's
// bytes, and rasterizes the same 101-glyph set the baked font carries
// (font_ttf.h: ASCII 32-126 plus the six Nordic letters) into an ATLAS
// laid out identically to a baked variant -- which is the whole trick.
// gfx.c, vga.c, the window server and every ring-3 client already know
// how to read that layout, so a runtime face changes where glyphs COME
// FROM and nothing else.
//
// **THE BAKED FONT IS THE FALLBACK AND IT IS NOT OPTIONAL.** Nothing
// here runs before the filesystem is mounted, and a console that cannot
// draw text until a disk font loads is a console that cannot report why
// the disk font did not load. font_ttf.c stays in the kernel image;
// font_face_atlas() returning NULL means "use it", and that is the
// state the machine boots in, panics in, and falls back to whenever a
// file is missing, malformed or too big.
//
// A face is named by its FILENAME without the extension, the same way a
// cursor theme is named by its directory (cursor_theme.c) -- so listing
// what is available is a directory listing and nothing has to parse the
// font's internal `name` table to answer `fonts`.
#include <stdint.h>

#define FONT_FACE_MAX       8   // faces the scan will remember
#define FONT_FACE_NAME_LEN  32
#define FONT_FACE_PATH_LEN  64  // FS_PATH_MAX

// The suffix that makes a file the BOLD MEMBER OF A FAMILY rather than
// a face of its own: dejavu-sans-mono.ttf and dejavu-sans-mono-bold.ttf
// are one face named `dejavu-sans-mono` with two weights, and `fontface`
// lists it once.
//
// A FILENAME CONVENTION RATHER THAN THE FONT'S OWN `name` TABLE, and
// that is the same call the face name itself already makes. Real
// systems read the metadata: fontconfig scans every file and indexes
// its family/weight/slant, and DirectWrite builds a family tree from
// the same tables. That is the right answer when a system must cope
// with whatever fonts a user has, and it costs a `name` table parser,
// an OS/2 usWeightClass reader, and a policy for the many files whose
// metadata disagrees with itself. Here the directory is small, seeded
// by the build, and already names faces by filename -- so one more
// suffix rule keeps the whole registry a directory listing.
#define FONT_BOLD_SUFFIX "-bold"

enum font_weight {
    FONT_WEIGHT_REGULAR = 0,
    FONT_WEIGHT_BOLD    = 1,
    FONT_WEIGHT_COUNT   = 2,
};

// What gfx.c draws from. Deliberately the same shape a baked
// font_ttf_variant has, plus the two things a baked variant cannot
// have: per-glyph advances, and kerning.
//
// The blob's three sections are laid out BACK TO BACK IN THIS ORDER and
// nothing may reorder them: glyphs, advances, kern. A ring-3 client is
// told where the glyphs and the advances start and DERIVES the kern
// table from `advances + count`, so the order is ABI. See
// WIN_REQ_FONT in abi/win_proto.h.
struct font_atlas {
    const uint8_t *glyphs;    // count * cell_w * cell_h coverage bytes
    const uint8_t *advances;  // count bytes: pixel advance per glyph

    // KERNING AS A DENSE count x count MATRIX OF SIGNED PIXELS, indexed
    // [left * count + right] by ATLAS SLOT (not by glyph id -- a client
    // has no cmap). Always present in a runtime atlas, all zeros for a
    // face that does not kern.
    //
    // Dense rather than a sorted pair list, which is what the font file
    // itself uses: 101 x 101 is 10 KB, against an atlas that is 14 KB at
    // 14px and ~640 KB at 64px, and it makes the lookup an array index
    // on a path that runs once per character drawn. A sorted list would
    // save a few KB and put a binary search in the inner loop of every
    // string measurement in both rings. It also makes the layout
    // UNCONDITIONAL, which is what lets a client derive the offset
    // instead of being told it.
    const int8_t *kern;

    int cell_w;               // the fixed cell: the WIDEST advance in the set
    int cell_h;
    int count;                // == FONT_TTF_GLYPH_COUNT
    int px;                   // em size this was rasterized at
    int baseline;             // rows from the cell top to the baseline
    int monospace;            // 1 when every advance is equal
    int weight;               // enum font_weight
    int synthetic;            // 1 when a bold was SMEARED rather than loaded
    uint64_t phys;            // page-aligned base, for WIN_REQ_FONT's mapping
    uint64_t bytes;           // total size of that allocation
};

struct font_face_info {
    char name[FONT_FACE_NAME_LEN];
    char path[FONT_FACE_PATH_LEN];
    uint64_t size;
    // The family's bold member, or "" when it has none -- in which case
    // bold is SYNTHESIZED (ttf_embolden()) rather than unavailable.
    char bold_path[FONT_FACE_PATH_LEN];
    int has_bold;
};

// Scans /usr/share/fonts. Cheap -- a directory listing, no file is
// opened. Safe to call before any face is selected, and safe to call
// again (a font dropped into the directory at runtime appears).
void font_face_init(void);

int font_face_count(void);
int font_face_info(int index, struct font_face_info *out);

// Makes `name` the active face, loading and validating its file.
// Returns 1 on success; 0 if the name is unknown, the file will not
// fit, or ttf_open() refuses it -- in which case the PREVIOUS face
// stays active rather than the machine losing its font mid-sentence.
// An empty name or NULL selects the baked font.
int font_face_select(const char *name);

// The active face's name, or "" when the baked font is in use.
const char *font_face_active(void);

// Rasterizes the active face at `px` in `weight` and returns the atlas,
// or NULL if no face is active or the build failed. Cached: asking
// twice for the same (face, weight, px) is free.
//
// A BOLD BUILD NEVER FAILS BECAUSE THE FAMILY HAS NO BOLD FILE. It
// falls back to emboldening the regular outlines, which is what GDI
// does for a family with no bold face and what Cairo and DirectWrite
// fall back to. `synthetic` on the returned atlas says which happened.
const struct font_atlas *font_face_build(int px, enum font_weight weight);

// The atlas gfx.c is currently drawing from at `weight`, or NULL when
// the baked font is in use (which has one weight and no advances).
const struct font_atlas *font_face_atlas_weight(enum font_weight weight);

// The regular-weight atlas -- font_face_atlas_weight(FONT_WEIGHT_REGULAR).
// Kept as its own name because most callers are weight-agnostic and
// spelling out the enum at every one of them reads as if the choice
// mattered there.
const struct font_atlas *font_face_atlas(void);

// Total bytes held by the atlas cache, for `meminfo`/`fonts`.
uint64_t font_face_atlas_bytes(void);

#endif
