#ifndef KERNEL_EDID_H
#define KERNEL_EDID_H

#include <stdint.h>

// EDID: what a monitor says about itself, parsed once into a struct the
// display layer keeps (Linux's drm_edid, in miniature). The parser is
// PURE -- bytes in, struct out, no hardware -- so one implementation
// serves the Intel AUX channel, virtio-gpu's GET_EDID, bochs's BAR and
// a KTEST on a canned blob.
//
// THE INVARIANT: a block that fails the header or the checksum is
// REJECTED, not read around. An EDID arrives over a 1 Mbit serial line
// or from a device model; a byte wrong anywhere makes every field after
// it a guess, and a guessed native timing is what a modeset would
// program.

#define EDID_BLOCK 128
#define EDID_MAX   (2 * EDID_BLOCK)   // the base block and one extension: all a reader asks for
#define EDID_NAME_MAX 14   // 13 characters and a terminator

// One detailed timing, in the EDID's own vocabulary (pixels and lines;
// the pixel clock in kHz). Blanking and sync are what a CRTC register
// wants: front porch = sync_off, sync width, and the rest of the blank.
struct edid_timing {
    uint32_t pixel_khz;
    uint16_t hactive, hblank, hsync_off, hsync_w;
    uint16_t vactive, vblank, vsync_off, vsync_w;
    uint16_t width_mm, height_mm;   // the image size this timing describes
    uint8_t  interlaced;
    uint8_t  hsync_pos, vsync_pos;  // polarity, meaningful for a digital separate sync
};

// A mode the monitor LISTS by size and refresh -- the established
// bitmap and the standard timings -- with no timing of its own: a
// driver that wants to set one looks the timing up (edid_dmt_timing()).
struct edid_mode {
    uint16_t w, h;
    uint8_t  hz;
};
#define EDID_MODES_MAX 24

struct display_edid {
    char     vendor[4];        // three letters, from the PNP id
    uint16_t product;
    uint32_t serial;
    uint8_t  week, year_1990;  // manufacture; year is offset from 1990
    uint8_t  version, revision;
    uint8_t  digital;
    uint16_t width_cm, height_cm;
    char     name[EDID_NAME_MAX];   // the monitor-name descriptor, or ""
    int      timing_count;          // detailed timings found, 0..4
    struct edid_timing timing[4];   // [0] is the preferred one
    int      mode_count;            // established + standard, as listed
    struct edid_mode mode[EDID_MODES_MAX];
    uint8_t  extensions;            // blocks after this one (CEA etc.), unread
};

// 1 and `out` filled, or 0 (bad header, short block, bad checksum) and
// `out` untouched. `len` is the bytes available. A CEA-861 extension in
// the second block adds its short video descriptors to `mode`; a bad
// extension is skipped, never a reason to reject the base block.
int edid_parse(const uint8_t *block, int len, struct display_edid *out);

// The refresh rate of a timing in millihertz (59940 for 59.94 Hz), or
// 0 for a timing with no total. Derived, so never stored.
uint32_t edid_refresh_mhz(const struct edid_timing *t);

// The VESA DMT timing for w x h at `hz`, from a small table of the sizes
// the display ladder offers; 1 and `out` filled, or 0 for a mode it has
// no timing for. Pure, like the parser.
int edid_dmt_timing(uint16_t w, uint16_t h, uint8_t hz, struct edid_timing *out);

// Totals, for comparing against a CRTC: active + blank.
static inline uint32_t edid_htotal(const struct edid_timing *t) { return (uint32_t)t->hactive + t->hblank; }
static inline uint32_t edid_vtotal(const struct edid_timing *t) { return (uint32_t)t->vactive + t->vblank; }

#endif
