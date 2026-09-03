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
    *out = e;
    return 1;
}

uint32_t edid_refresh_mhz(const struct edid_timing *t) {
    if (!t) return 0;
    uint32_t total = edid_htotal(t) * edid_vtotal(t);
    if (!total) return 0;
    // kHz * 1000 / total is Hz*1000 with no overflow below ~4 GHz.
    uint64_t hz_m = (uint64_t)t->pixel_khz * 1000000ull / total;
    return (uint32_t)hz_m;
}
