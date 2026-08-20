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

// What gfx.c draws from. Deliberately the same shape a baked
// font_ttf_variant has, plus the one thing a baked variant cannot have:
// per-glyph advances.
struct font_atlas {
    const uint8_t *glyphs;    // count * cell_w * cell_h coverage bytes
    const uint8_t *advances;  // count bytes: pixel advance per glyph
    int cell_w;               // the fixed cell: the WIDEST advance in the set
    int cell_h;
    int count;                // == FONT_TTF_GLYPH_COUNT
    int px;                   // em size this was rasterized at
    int baseline;             // rows from the cell top to the baseline
    int monospace;            // 1 when every advance is equal
    uint64_t phys;            // page-aligned base, for WIN_REQ_FONT's mapping
    uint64_t bytes;           // total size of that allocation
};

struct font_face_info {
    char name[FONT_FACE_NAME_LEN];
    char path[FONT_FACE_PATH_LEN];
    uint64_t size;
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

// Rasterizes the active face at `px` and returns the atlas, or NULL if
// no face is active or the build failed. Cached: asking twice for the
// same (face, px) is free.
const struct font_atlas *font_face_build(int px);

// The atlas gfx.c is currently drawing from, or NULL for the baked font.
const struct font_atlas *font_face_atlas(void);

// Total bytes held by the atlas cache, for `meminfo`/`fonts`.
uint64_t font_face_atlas_bytes(void);

#endif
