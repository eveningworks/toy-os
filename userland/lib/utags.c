// See utags.h. ID3v2's frame layouts are id3.org's 2.2/2.3/2.4
// documents; the MIDI walk is the SMF 1.0 event grammar.
#include "lib/utags.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID3_MAX   (4u * 1024u * 1024u)   // a tag bigger than this is refused, art and all
#define MIDI_HEAD (64u * 1024u)          // the first track's name is near the start

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// 4 bytes of 7 bits each; -1 if a byte has its top bit set.
static long syncsafe(const uint8_t *p) {
    if ((p[0] | p[1] | p[2] | p[3]) & 0x80) return -1;
    return (long)p[0] << 21 | (long)p[1] << 14 | (long)p[2] << 7 | p[3];
}

size_t utags_id3_size(const uint8_t *head, size_t len) {
    if (len < 10 || memcmp(head, "ID3", 3) != 0 || head[3] < 2 || head[3] > 4) return 0;
    long body = syncsafe(head + 6);
    if (body < 0) return 0;
    return 10 + (size_t)body + ((head[3] == 4 && (head[5] & 0x10)) ? 10 : 0);
}

// --- text -------------------------------------------------------------------

static void put(char *dst, size_t *n, unsigned cp) {
    if (*n + 1 >= UTAGS_TEXT_MAX) return;
    dst[(*n)++] = (char)(cp && cp < 0x100 ? cp : '?');
}

// One text value in ID3 encoding `enc` (0 Latin-1, 1 UTF-16 with BOM,
// 2 UTF-16BE, 3 UTF-8) to Latin-1, up to its terminator. 0 when the
// encoding is unknown -- the field stays empty rather than misread.
static int decode(char *dst, int enc, const uint8_t *s, size_t len) {
    size_t n = 0;
    if (enc == 0) {
        for (size_t i = 0; i < len && s[i]; i++) put(dst, &n, s[i]);
    } else if (enc == 3) {
        for (size_t i = 0; i < len && s[i];) {
            unsigned c = s[i], cp, more;
            if (c < 0x80) { cp = c; more = 0; }
            else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; more = 1; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; more = 2; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; more = 3; }
            else { cp = '?'; more = 0; }
            i++;
            for (unsigned k = 0; k < more; k++, i++) {
                if (i >= len || (s[i] & 0xC0) != 0x80) { cp = '?'; break; }
                cp = cp << 6 | (s[i] & 0x3F);
            }
            put(dst, &n, cp);
        }
    } else if (enc == 1 || enc == 2) {
        int big = enc == 2;
        size_t i = 0;
        if (enc == 1) {
            if (len < 2) { dst[0] = '\0'; return 1; }
            if (s[0] == 0xFE && s[1] == 0xFF) big = 1;
            else if (s[0] == 0xFF && s[1] == 0xFE) big = 0;
            else return 0;
            i = 2;
        }
        for (; i + 1 < len; i += 2) {
            unsigned u = big ? (unsigned)s[i] << 8 | s[i + 1] : (unsigned)s[i + 1] << 8 | s[i];
            if (!u) break;
            put(dst, &n, (u >= 0xD800 && u < 0xE000) ? '?' : u);
        }
    } else {
        return 0;
    }
    dst[n] = '\0';
    return 1;
}

// Where a string in encoding `enc` ends, terminator included: one zero
// byte, or an ALIGNED pair for UTF-16. `len` when unterminated.
static size_t skip_string(int enc, const uint8_t *s, size_t len) {
    if (enc == 1 || enc == 2) {
        for (size_t i = 0; i + 1 < len; i += 2) if (!s[i] && !s[i + 1]) return i + 2;
        return len;
    }
    for (size_t i = 0; i < len; i++) if (!s[i]) return i + 1;
    return len;
}

// --- ID3v2 ------------------------------------------------------------------

static int text_frame(char *dst, const uint8_t *body, size_t len) {
    if (len < 2 || dst[0]) return 0;           // empty, or already set by an earlier frame
    return decode(dst, body[0], body + 1, len - 1) && dst[0];
}

// APIC (2.3/2.4: a MIME string) or PIC (2.2: three letters). The front
// cover (type 3) wins over any picture seen before it.
static int picture(struct utags *t, int v22, const uint8_t *b, size_t len, int *front) {
    if (len < 4) return 0;
    int enc = b[0];
    size_t i = 1;
    if (v22) i += 3;
    else i += skip_string(0, b + i, len - i);
    if (i >= len) return 0;
    int type = b[i++];
    if (enc > 3) return 0;
    i += skip_string(enc, b + i, len - i);
    if (i >= len || (t->art && (*front || type != 3))) return 0;
    uint8_t *copy = malloc(len - i);
    if (!copy) return 0;
    memcpy(copy, b + i, len - i);
    free(t->art);
    t->art = copy;
    t->art_len = len - i;
    *front = type == 3;
    return 1;
}

int utags_from_id3v2(const uint8_t *buf, size_t len, struct utags *t, int want_art) {
    size_t total = utags_id3_size(buf, len);
    if (!total || total > len) return 0;
    int ver = buf[3], flags = buf[5];
    // An unsynchronised tag needs undoing first; refused, as nothing
    // that writes tags today sets it.
    if (flags & 0x80) return 0;
    size_t end = 10 + (size_t)syncsafe(buf + 6), i = 10;
    if (ver >= 3 && (flags & 0x40)) {           // an extended header: skip it
        if (i + 4 > end) return 0;
        long ext = ver == 4 ? syncsafe(buf + i) : (long)be32(buf + i) + 4;
        if (ext < 4 || i + (size_t)ext > end) return 0;
        i += (size_t)ext;
    }
    int found = 0, front = 0;
    size_t hdr = ver == 2 ? 6 : 10;
    while (i + hdr <= end) {
        const uint8_t *f = buf + i;
        if (!f[0]) break;                        // padding
        size_t fsize;
        unsigned fflags = 0;
        if (ver == 2) {
            fsize = (size_t)f[3] << 16 | (size_t)f[4] << 8 | f[5];
        } else if (ver == 3) {
            fsize = be32(f + 4);
            fflags = f[9];
        } else {
            long s = syncsafe(f + 4);
            if (s < 0) break;
            fsize = (size_t)s;
            fflags = f[9];
        }
        if (fsize > end - i - hdr) break;        // a frame past the tag: stop, keep what we have
        size_t step = hdr + fsize;               // before the flag bytes below shrink fsize
        const uint8_t *body = f + hdr;
        // Compressed, encrypted or unsynchronised frames are skipped.
        int skip = ver == 3 ? (fflags & 0xC0) != 0 : ver == 4 ? (fflags & 0x0E) != 0 : 0;
        if (ver == 3 && (fflags & 0x20) && fsize) { body++; fsize--; }       // group id
        if (ver == 4 && (fflags & 0x01)) {                                   // data length
            if (fsize < 4) skip = 1; else { body += 4; fsize -= 4; }
        }
        if (!skip) {
            char id[5] = { (char)f[0], (char)f[1], (char)f[2], ver == 2 ? 0 : (char)f[3], 0 };
            if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) found |= text_frame(t->title, body, fsize) ? UTAGS_TITLE : 0;
            else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) found |= text_frame(t->artist, body, fsize) ? UTAGS_ARTIST : 0;
            else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) found |= text_frame(t->album, body, fsize) ? UTAGS_ALBUM : 0;
            else if (!strcmp(id, "TLEN") || !strcmp(id, "TLE")) {
                char ms[UTAGS_TEXT_MAX] = "";
                if (fsize >= 2 && decode(ms, body[0], body + 1, fsize - 1)) {
                    // Digits only: a length that is not a number is ignored.
                    unsigned long v = 0;
                    const char *c = ms;
                    while (*c >= '0' && *c <= '9' && v < 100000000ul) v = v * 10 + (unsigned long)(*c++ - '0');
                    if (!*c && c != ms) { t->length_ms = (uint32_t)v; found |= UTAGS_LENGTH; }
                }
            }
            else if (want_art && (!strcmp(id, "APIC") || !strcmp(id, "PIC")))
                found |= picture(t, ver == 2, body, fsize, &front) ? UTAGS_ART : 0;
        }
        i += step;
    }
    return found;
}

// --- ID3v1 ------------------------------------------------------------------

static int v1_field(char *dst, const uint8_t *s, size_t n) {
    if (dst[0]) return 0;
    char tmp[31];
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    size_t l = strlen(tmp);
    while (l && tmp[l - 1] == ' ') tmp[--l] = '\0';
    if (!l) return 0;
    decode(dst, 0, (const uint8_t *)tmp, l);
    return 1;
}

int utags_from_id3v1(const uint8_t *tr, struct utags *t) {
    if (memcmp(tr, "TAG", 3) != 0) return 0;
    return (v1_field(t->title, tr + 3, 30) ? UTAGS_TITLE : 0) |
           (v1_field(t->artist, tr + 33, 30) ? UTAGS_ARTIST : 0) |
           (v1_field(t->album, tr + 63, 30) ? UTAGS_ALBUM : 0);
}

// --- MIDI -------------------------------------------------------------------

// A variable-length quantity; -1 past `end` or longer than four bytes.
static long vlq(const uint8_t *b, size_t end, size_t *i) {
    long v = 0;
    for (int k = 0; k < 4; k++) {
        if (*i >= end) return -1;
        uint8_t c = b[(*i)++];
        v = v << 7 | (c & 0x7F);
        if (!(c & 0x80)) return v;
    }
    return -1;
}

int utags_from_midi(const uint8_t *b, size_t len, struct utags *t) {
    if (len < 22 || memcmp(b, "MThd", 4) != 0) return 0;
    size_t i = 8 + be32(b + 4);
    if (i + 8 > len || memcmp(b + i, "MTrk", 4) != 0) return 0;
    size_t end = i + 8 + be32(b + i + 4);
    if (end > len) end = len;                  // a truncated read: walk what is here
    i += 8;
    uint8_t status = 0;
    for (int events = 0; i < end && events < 4096; events++) {
        if (vlq(b, end, &i) < 0 || i >= end) return 0;
        uint8_t c = b[i];
        if (c & 0x80) { status = c; i++; }
        else if (!status) return 0;            // running status with none to run on
        if (status == 0xFF) {
            if (i >= end) return 0;
            uint8_t type = b[i++];
            long n = vlq(b, end, &i);
            if (n < 0 || (size_t)n > end - i) return 0;
            if (type == 0x03 && !t->title[0]) {
                decode(t->title, 0, b + i, (size_t)n);
                return t->title[0] ? UTAGS_TITLE : 0;
            }
            if (type == 0x2F) return 0;        // end of track, no name
            i += (size_t)n;
            status = 0;                         // meta events cancel running status
        } else if (status == 0xF0 || status == 0xF7) {
            long n = vlq(b, end, &i);
            if (n < 0 || (size_t)n > end - i) return 0;
            i += (size_t)n;
            status = 0;
        } else {
            i += ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) ? 1 : 2;
        }
    }
    return 0;
}

// --- the file -----------------------------------------------------------------

int utags_read(const char *path, struct utags *t, int want_art) {
    memset(t, 0, sizeof *t);
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    uint8_t head[10];
    int found = 0;
    if (fread(head, 1, sizeof head, fp) == sizeof head) {
        size_t tag = utags_id3_size(head, sizeof head);
        if (tag && tag <= ID3_MAX) {
            uint8_t *buf = malloc(tag);
            if (buf) {
                memcpy(buf, head, sizeof head);
                if (fread(buf + 10, 1, tag - 10, fp) == tag - 10)
                    found |= utags_from_id3v2(buf, tag, t, want_art);
                free(buf);
            }
        } else if (!memcmp(head, "MThd", 4)) {
            uint8_t *buf = malloc(MIDI_HEAD);
            if (buf) {
                memcpy(buf, head, sizeof head);
                size_t n = 10 + fread(buf + 10, 1, MIDI_HEAD - 10, fp);
                found |= utags_from_midi(buf, n, t);
                free(buf);
            }
        }
        // ID3v1 fills what v2 did not, on any file that ends with one.
        uint8_t tr[128];
        if (fseek(fp, -128, SEEK_END) == 0 && fread(tr, 1, sizeof tr, fp) == sizeof tr)
            found |= utags_from_id3v1(tr, t);
    }
    fclose(fp);
    return found;
}

void utags_free(struct utags *t) {
    free(t->art);
    t->art = 0;
    t->art_len = 0;
}
