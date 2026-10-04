// KTESTs for the EDID parser: a canned base block for a 15.6" eDP
// panel, and the ways a block is refused. Runs on every machine, which
// is the point of the parser being pure.
#include "edid.h"
#include "display.h"
#include "string.h"
#include "ktest.h"

// LGD 0x0469, "LP156WF6", 1920x1080 at 138.5 MHz: h 1920 48 32 160,
// v 1080 3 5 31, 344x194 mm, digital separate sync, +hsync -vsync.
static const uint8_t PANEL[EDID_BLOCK] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x30, 0xE4, 0x69, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x1A, 0x01, 0x04, 0x95, 0x22, 0x13, 0x78, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1A, 0x36, 0x80, 0xA0, 0x70, 0x38, 0x1F, 0x40, 0x30, 0x20,
    0x35, 0x00, 0x58, 0xC2, 0x10, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x4C, 0x50, 0x31,
    0x35, 0x36, 0x57, 0x46, 0x36, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x29,
};

KTEST("edid", "the base block parses: vendor, product, name, size") {
    struct display_edid e;
    KTEST_ASSERT_EQ(edid_parse(PANEL, EDID_BLOCK, &e), 1);
    KTEST_ASSERT(k_strcmp(e.vendor, "LGD") == 0);
    KTEST_ASSERT_EQ(e.product, 0x0469u);
    KTEST_ASSERT(k_strcmp(e.name, "LP156WF6") == 0);   // LF-terminated, padding dropped
    KTEST_ASSERT_EQ(e.digital, 1);
    KTEST_ASSERT_EQ(e.width_cm, 34u);
    KTEST_ASSERT_EQ(e.height_cm, 19u);
    KTEST_ASSERT_EQ(e.year_1990, 26);
    KTEST_ASSERT_EQ(e.version, 1);
    KTEST_ASSERT_EQ(e.revision, 4);
}

KTEST("edid", "without a name descriptor the longest 0xFE text is the name") {
    uint8_t b[EDID_BLOCK];
    k_memcpy(b, PANEL, EDID_BLOCK);
    b[72 + 3] = 0xFE;          // the same text, as "unspecified text"
    b[127] = (uint8_t)(b[127] - 2);
    // A second, shorter 0xFE text in descriptor 3 must not win.
    b[90 + 3] = 0xFE;
    b[90 + 5] = 'A'; b[90 + 6] = 'U'; b[90 + 7] = 'O'; b[90 + 8] = 0x0A;
    b[127] = (uint8_t)(b[127] - (0xFE - 0x10) - 'A' - 'U' - 'O' - 0x0A);
    struct display_edid e;
    KTEST_ASSERT_EQ(edid_parse(b, EDID_BLOCK, &e), 1);
    KTEST_ASSERT(k_strcmp(e.name, "LP156WF6") == 0);
}

KTEST("edid", "the detailed timing unpacks every nibble") {
    struct display_edid e;
    KTEST_ASSERT_EQ(edid_parse(PANEL, EDID_BLOCK, &e), 1);
    KTEST_ASSERT_EQ(e.timing_count, 1);
    const struct edid_timing *t = &e.timing[0];
    KTEST_ASSERT_EQ(t->pixel_khz, 138500u);
    KTEST_ASSERT_EQ(t->hactive, 1920u);
    KTEST_ASSERT_EQ(t->hblank, 160u);
    KTEST_ASSERT_EQ(t->hsync_off, 48u);
    KTEST_ASSERT_EQ(t->hsync_w, 32u);
    KTEST_ASSERT_EQ(t->vactive, 1080u);
    KTEST_ASSERT_EQ(t->vblank, 31u);
    KTEST_ASSERT_EQ(t->vsync_off, 3u);
    KTEST_ASSERT_EQ(t->vsync_w, 5u);
    KTEST_ASSERT_EQ(t->width_mm, 344u);
    KTEST_ASSERT_EQ(t->height_mm, 194u);
    KTEST_ASSERT_EQ(t->interlaced, 0);
    KTEST_ASSERT_EQ(t->hsync_pos, 1);
    KTEST_ASSERT_EQ(t->vsync_pos, 0);
    KTEST_ASSERT_EQ(edid_htotal(t), 2080u);
    KTEST_ASSERT_EQ(edid_vtotal(t), 1111u);
    // 138.5 MHz / (2080 * 1111) = 59.93 Hz
    uint32_t mhz = edid_refresh_mhz(t);
    KTEST_ASSERT(mhz >= 59920 && mhz <= 59940);
}

KTEST("edid", "a bad header, a bad checksum or a short block is refused, untouched") {
    uint8_t b[EDID_BLOCK];
    struct display_edid e;
    k_memset(&e, 0x5A, sizeof e);
    k_memcpy(b, PANEL, EDID_BLOCK);
    b[127] ^= 1;
    KTEST_ASSERT_EQ(edid_parse(b, EDID_BLOCK, &e), 0);
    k_memcpy(b, PANEL, EDID_BLOCK);
    b[1] = 0x00;
    KTEST_ASSERT_EQ(edid_parse(b, EDID_BLOCK, &e), 0);
    KTEST_ASSERT_EQ(edid_parse(PANEL, EDID_BLOCK - 1, &e), 0);
    KTEST_ASSERT_EQ(edid_parse(0, EDID_BLOCK, &e), 0);
    KTEST_ASSERT_EQ(e.timing_count, 0x5A5A5A5A);   // never written
}

KTEST("edid", "the active display's EDID, when there is one, names a timing the screen can show") {
    const struct display_edid *e = display_edid();
    if (!e) KTEST_SKIP("the active display driver read no EDID");
    KTEST_ASSERT(e->timing_count >= 1);
    KTEST_ASSERT(e->timing[0].hactive >= 640);
    KTEST_ASSERT(e->timing[0].vactive >= 480);
    KTEST_ASSERT(e->timing[0].pixel_khz > 0);
}

// The mode lists, on the canned block with its established and standard
// bytes rewritten (and the checksum fixed): 640x480@60 and 1024x768@60
// from the bitmap; 1920x1080@60 (16:9), 1280x1024@60 (5:4), 1680x1050@60
// (16:10 in EDID 1.3+) and an unused slot from the standard timings.
KTEST("edid", "established and standard timings become listed modes") {
    uint8_t b[EDID_BLOCK];
    k_memcpy(b, PANEL, EDID_BLOCK);
    b[35] = 0x20; b[36] = 0x08; b[37] = 0;
    for (int i = 38; i < 54; i++) b[i] = 0x01;
    b[38] = (1920 / 8) - 31; b[39] = 0xC0;
    b[40] = (1280 / 8) - 31; b[41] = 0x80;
    b[42] = (1680 / 8) - 31; b[43] = 0x00;
    uint8_t sum = 0;
    for (int i = 0; i < EDID_BLOCK - 1; i++) sum = (uint8_t)(sum + b[i]);
    b[127] = (uint8_t)(0x100 - sum);
    struct display_edid e;
    KTEST_ASSERT_EQ(edid_parse(b, EDID_BLOCK, &e), 1);
    KTEST_ASSERT_EQ(e.mode_count, 5);
    KTEST_ASSERT(e.mode[0].w == 640 && e.mode[0].h == 480 && e.mode[0].hz == 60);
    KTEST_ASSERT(e.mode[1].w == 1024 && e.mode[1].h == 768);
    KTEST_ASSERT(e.mode[2].w == 1920 && e.mode[2].h == 1080 && e.mode[2].hz == 60);
    KTEST_ASSERT(e.mode[3].w == 1280 && e.mode[3].h == 1024);
    KTEST_ASSERT(e.mode[4].w == 1680 && e.mode[4].h == 1050);
}

// DMT 0x52, the timing a real 1920x1080@60 is set with: 148.5 MHz,
// 2200 x 1125 total, both syncs positive.
KTEST("edid", "a listed mode's timing comes from the DMT table") {
    struct edid_timing t;
    KTEST_ASSERT_EQ(edid_dmt_timing(1920, 1080, 60, &t), 1);
    KTEST_ASSERT_EQ(t.pixel_khz, 148500u);
    KTEST_ASSERT_EQ(edid_htotal(&t), 2200u);
    KTEST_ASSERT_EQ(edid_vtotal(&t), 1125u);
    KTEST_ASSERT(t.hsync_pos && t.vsync_pos);
    KTEST_ASSERT(edid_refresh_mhz(&t) == 60000);
    KTEST_ASSERT_EQ(edid_dmt_timing(1920, 1080, 75, &t), 0);   // not in the table
}

// A CEA-861 extension's video data block: VIC 16 (1920x1080@60), VIC 4
// with the native flag (0x84), VIC 3 (720x480, no DMT timing, dropped)
// and VIC 16 again (listed once). A corrupt extension adds nothing and
// still leaves the base block parsed.
KTEST("edid", "a CEA extension's short video descriptors become listed modes") {
    static uint8_t b[EDID_MAX];
    k_memcpy(b, PANEL, EDID_BLOCK);
    b[126] = 1;
    uint8_t sum = 0;
    for (int i = 0; i < EDID_BLOCK - 1; i++) sum = (uint8_t)(sum + b[i]);
    b[127] = (uint8_t)(0x100 - sum);
    uint8_t *x = b + EDID_BLOCK;
    k_memset(x, 0, EDID_BLOCK);
    x[0] = 0x02; x[1] = 0x03; x[2] = 9;
    x[4] = (2 << 5) | 4; x[5] = 16; x[6] = 0x84; x[7] = 3; x[8] = 16;
    sum = 0;
    for (int i = 0; i < EDID_BLOCK - 1; i++) sum = (uint8_t)(sum + x[i]);
    x[127] = (uint8_t)(0x100 - sum);
    struct display_edid e;
    KTEST_ASSERT_EQ(edid_parse(b, EDID_MAX, &e), 1);
    KTEST_ASSERT_EQ(e.extensions, 1);
    KTEST_ASSERT_EQ(e.mode_count, 2);
    KTEST_ASSERT(e.mode[0].w == 1920 && e.mode[0].h == 1080 && e.mode[0].hz == 60);
    KTEST_ASSERT(e.mode[1].w == 1280 && e.mode[1].h == 720);
    x[127] ^= 1;   // corrupt the extension
    KTEST_ASSERT_EQ(edid_parse(b, EDID_MAX, &e), 1);
    KTEST_ASSERT_EQ(e.mode_count, 0);
}
