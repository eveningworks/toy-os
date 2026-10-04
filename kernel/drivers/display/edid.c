// The EDID base-block parser. See edid.h for the contract.
//
// Byte offsets are the VESA E-EDID 1.4 base block's; the detailed
// timing descriptor's nibble packing is the part worth a second look
// before editing, since a swapped high nibble produces a timing that
// is plausible and wrong.
#include "edid.h"
#include "string.h"

// driver-none: a parser, not a driver

static const uint8_t EDID_HEADER[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };

static void parse_dtd(const uint8_t *d, struct edid_timing *t) {
    t->pixel_khz = ((uint32_t)d[0] | ((uint32_t)d[1] << 8)) * 10;
    t->hactive   = (uint16_t)(d[2] | ((d[4] & 0xF0) << 4));
    t->hblank    = (uint16_t)(d[3] | ((d[4] & 0x0F) << 8));
    t->vactive   = (uint16_t)(d[5] | ((d[7] & 0xF0) << 4));
    t->vblank    = (uint16_t)(d[6] | ((d[7] & 0x0F) << 8));
    t->hsync_off = (uint16_t)(d[8]  | ((d[11] & 0xC0) << 2));
    t->hsync_w   = (uint16_t)(d[9]  | ((d[11] & 0x30) << 4));
    t->vsync_off = (uint16_t)((d[10] >> 4) | ((d[11] & 0x0C) << 2));
    t->vsync_w   = (uint16_t)((d[10] & 0x0F) | ((d[11] & 0x03) << 4));
    t->width_mm  = (uint16_t)(d[12] | ((d[14] & 0xF0) << 4));
    t->height_mm = (uint16_t)(d[13] | ((d[14] & 0x0F) << 8));
    t->interlaced = (d[17] & 0x80) ? 1 : 0;
    // Polarity bits only mean something for a digital separate sync
    // (bits 4:3 == 11); for the analog types they are level flags.
    int separate = (d[17] & 0x18) == 0x18;
    t->vsync_pos = (separate && (d[17] & 0x04)) ? 1 : 0;
    t->hsync_pos = (separate && (d[17] & 0x02)) ? 1 : 0;
}

// A descriptor's text runs to 0x0A and is space-padded after it.
static void copy_text(const uint8_t *d, char *out, int cap) {
    int n = 0;
    for (int i = 5; i < 18 && n < cap - 1; i++) {
        if (d[i] == 0x0A) break;
        out[n++] = (char)d[i];
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
}

// CEA-861 short video descriptors (the video data block, tag 2): a VIC
// each, bit 7 the "native" flag for VICs 1-64. A VIC is kept when the
// DMT table can time its size -- the same timing under another name --
// so this table names sizes and edid_dmt_timing() decides.
static void parse_cea(const uint8_t *x, struct display_edid *e) {
    static const struct { uint8_t vic; struct edid_mode m; } VIC[] = {
        { 1, { 640, 480, 60 } }, { 2, { 720, 480, 60 } }, { 3, { 720, 480, 60 } },
        { 4, { 1280, 720, 60 } }, { 16, { 1920, 1080, 60 } }, { 17, { 720, 576, 50 } },
        { 18, { 720, 576, 50 } }, { 19, { 1280, 720, 50 } }, { 31, { 1920, 1080, 50 } },
    };
    struct edid_timing scratch;
    uint8_t sum = 0;
    for (int i = 0; i < EDID_BLOCK; i++) sum = (uint8_t)(sum + x[i]);
    if (x[0] != 0x02 || sum != 0) return;
    int end = x[2] < 4 || x[2] > EDID_BLOCK - 1 ? 4 : x[2];
    for (int i = 4; i < end; ) {
        int tag = x[i] >> 5, n = x[i] & 0x1F;
        if (i + 1 + n > end) break;
        for (int j = 0; tag == 2 && j < n; j++) {
            uint8_t v = x[i + 1 + j];
            uint8_t vic = (v & 0x80) && (v & 0x7F) <= 64 ? (uint8_t)(v & 0x7F) : v;
            for (unsigned k = 0; k < sizeof VIC / sizeof VIC[0]; k++) {
                if (VIC[k].vic != vic || e->mode_count >= EDID_MODES_MAX) continue;
                if (!edid_dmt_timing(VIC[k].m.w, VIC[k].m.h, VIC[k].m.hz, &scratch)) continue;
                int dup = 0;
                for (int m = 0; m < e->mode_count; m++)
                    dup |= e->mode[m].w == VIC[k].m.w && e->mode[m].h == VIC[k].m.h &&
                           e->mode[m].hz == VIC[k].m.hz;
                if (!dup) e->mode[e->mode_count++] = VIC[k].m;
            }
        }
        i += 1 + n;
    }
}

int edid_parse(const uint8_t *b, int len, struct display_edid *out) {
    if (!b || !out || len < EDID_BLOCK) return 0;
    if (k_memcmp(b, EDID_HEADER, 8) != 0) return 0;
    uint8_t sum = 0;
    for (int i = 0; i < EDID_BLOCK; i++) sum = (uint8_t)(sum + b[i]);
    if (sum != 0) return 0;

    struct display_edid e;
    k_memset(&e, 0, sizeof e);
    uint16_t id = (uint16_t)((b[8] << 8) | b[9]);
    e.vendor[0] = (char)('A' - 1 + ((id >> 10) & 0x1F));
    e.vendor[1] = (char)('A' - 1 + ((id >> 5) & 0x1F));
    e.vendor[2] = (char)('A' - 1 + (id & 0x1F));
    e.vendor[3] = 0;
    e.product = (uint16_t)(b[10] | (b[11] << 8));
    e.serial = (uint32_t)b[12] | ((uint32_t)b[13] << 8) | ((uint32_t)b[14] << 16) | ((uint32_t)b[15] << 24);
    e.week = b[16];
    e.year_1990 = b[17];
    e.version = b[18];
    e.revision = b[19];
    e.digital = (b[20] & 0x80) ? 1 : 0;
    e.width_cm = b[21];
    e.height_cm = b[22];

    // The name is the 0xFC descriptor. A panel with none (AUO's, for
    // one) carries its model in a 0xFE "unspecified text" descriptor,
    // so the LONGEST such text stands in -- the short one is usually
    // the vendor's letters again.
    char text[EDID_NAME_MAX] = "";
    for (int i = 0; i < 4; i++) {
        const uint8_t *d = b + 54 + i * 18;
        if (d[0] || d[1]) {
            parse_dtd(d, &e.timing[e.timing_count++]);
        } else if (d[3] == 0xFC && !e.name[0]) {
            copy_text(d, e.name, EDID_NAME_MAX);
        } else if (d[3] == 0xFE) {
            char t[EDID_NAME_MAX];
            copy_text(d, t, EDID_NAME_MAX);
            if (k_strlen(t) > k_strlen(text)) k_memcpy(text, t, sizeof t);
        }
    }
    if (!e.name[0]) k_memcpy(e.name, text, sizeof text);

    // The established bitmap (bytes 35-37, EDID 1.3 table 3.18), the
    // modes worth setting: interlaced and Mac-only ones are skipped.
    static const struct { uint8_t byte, bit; struct edid_mode m; } EST[] = {
        { 35, 5, { 640, 480, 60 } }, { 35, 2, { 640, 480, 75 } },
        { 35, 0, { 800, 600, 60 } }, { 36, 6, { 800, 600, 75 } },
        { 36, 3, { 1024, 768, 60 } }, { 36, 1, { 1024, 768, 75 } },
        { 36, 0, { 1280, 1024, 75 } },
    };
    for (unsigned i = 0; i < sizeof EST / sizeof EST[0]; i++)
        if ((b[EST[i].byte] >> EST[i].bit) & 1 && e.mode_count < EDID_MODES_MAX)
            e.mode[e.mode_count++] = EST[i].m;
    // Standard timings (bytes 38-53): width/8 - 31, an aspect code, and
    // refresh - 60. 0x0101 (or 0x0000) is an unused slot. Aspect 00 is
    // 16:10 from EDID 1.3 on, 1:1 before.
    for (int i = 0; i < 8; i++) {
        uint8_t x = b[38 + 2 * i], a = b[39 + 2 * i];
        if ((x == 1 && a == 1) || x == 0 || e.mode_count >= EDID_MODES_MAX) continue;
        uint16_t w = (uint16_t)((x + 31) * 8), h;
        switch (a >> 6) {
        case 0: h = (e.version > 1 || e.revision >= 3) ? (uint16_t)(w * 10 / 16) : w; break;
        case 1: h = (uint16_t)(w * 3 / 4); break;
        case 2: h = (uint16_t)(w * 4 / 5); break;
        default: h = (uint16_t)(w * 9 / 16); break;
        }
        e.mode[e.mode_count++] = (struct edid_mode){ w, h, (uint8_t)((a & 0x3F) + 60) };
    }
    e.extensions = b[126];
    if (len >= EDID_MAX && e.extensions) parse_cea(b + EDID_BLOCK, &e);
    k_memcpy(out, &e, sizeof e);   // not `*out = e`: that is a memcpy call, and there is none
    return 1;
}

// VESA DMT (v1.0 r13) for the display ladder's sizes at 60 Hz. pixel
// kHz; h: active, front porch, sync, back porch; v the same; polarity.
static const struct {
    uint16_t w, h; uint8_t hz; uint32_t khz;
    uint16_t hfp, hs, hbp, vfp, vs, vbp; uint8_t hpos, vpos;
} DMT[] = {
    {  640,  480, 60,  25175, 16,  96,  48, 10, 2, 33, 0, 0 },   // 0x04
    {  800,  600, 60,  40000, 40, 128,  88,  1, 4, 23, 1, 1 },   // 0x09
    { 1024,  768, 60,  65000, 24, 136, 160,  3, 6, 29, 0, 0 },   // 0x10
    { 1280,  720, 60,  74250, 110, 40, 220,  5, 5, 20, 1, 1 },   // 0x55
    { 1280, 1024, 60, 108000, 48, 112, 248,  1, 3, 38, 1, 1 },   // 0x23
    { 1366,  768, 60,  85500, 70, 143, 213,  3, 3, 24, 1, 1 },   // 0x51
    { 1600,  900, 60, 108000, 24,  80,  96,  1, 3, 96, 1, 1 },   // 0x53, reduced blanking
    { 1920, 1080, 60, 148500, 88,  44, 148,  4, 5, 36, 1, 1 },   // 0x52
};

int edid_dmt_timing(uint16_t w, uint16_t h, uint8_t hz, struct edid_timing *out) {
    for (unsigned i = 0; i < sizeof DMT / sizeof DMT[0]; i++) {
        if (DMT[i].w != w || DMT[i].h != h || DMT[i].hz != hz) continue;
        k_memset(out, 0, sizeof *out);
        out->pixel_khz = DMT[i].khz;
        out->hactive = w;
        out->hsync_off = DMT[i].hfp;
        out->hsync_w = DMT[i].hs;
        out->hblank = (uint16_t)(DMT[i].hfp + DMT[i].hs + DMT[i].hbp);
        out->vactive = h;
        out->vsync_off = DMT[i].vfp;
        out->vsync_w = DMT[i].vs;
        out->vblank = (uint16_t)(DMT[i].vfp + DMT[i].vs + DMT[i].vbp);
        out->hsync_pos = DMT[i].hpos;
        out->vsync_pos = DMT[i].vpos;
        return 1;
    }
    return 0;
}

uint32_t edid_refresh_mhz(const struct edid_timing *t) {
    if (!t) return 0;
    uint32_t total = edid_htotal(t) * edid_vtotal(t);
    if (!total) return 0;
    // kHz * 1000 / total is Hz*1000 with no overflow below ~4 GHz.
    uint64_t hz_m = (uint64_t)t->pixel_khz * 1000000ull / total;
    return (uint32_t)hz_m;
}
