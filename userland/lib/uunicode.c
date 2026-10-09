// Unicode names, blocks and UTF-8 (lib/uunicode.h).
#include "lib/uunicode.h"
#include "lib/ufile.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define NAMES_PATH  "/usr/share/unicode/names"
#define BLOCKS_PATH "/usr/share/unicode/blocks"
#define NAMES_MAX   (1u << 20)

struct entry { uint32_t cp; const char *name; };

static char *g_names_text, *g_blocks_text;
static struct entry *g_ent;
static int g_count;
static struct uunicode_block *g_blocks;
static int g_nblocks;
static int g_loaded;

static int hex(const char *s, const char **end, uint32_t *out) {
    uint32_t v = 0;
    int n = 0;
    for (; isxdigit((unsigned char)*s) && n < 8; s++, n++)
        v = v * 16 + (uint32_t)(isdigit((unsigned char)*s) ? *s - '0' : (tolower((unsigned char)*s) - 'a' + 10));
    *end = s;
    *out = v;
    return n;
}

// A whole small text file in a fresh, NUL-terminated buffer.
static char *slurp(const char *path, size_t *len) {
    uint8_t *raw;
    size_t n;
    if (ufile_slurp(path, NAMES_MAX, &raw, &n) != UFILE_OK) return 0;
    char *buf = malloc(n + 1);
    if (buf) { memcpy(buf, raw, n); buf[n] = '\0'; *len = n; }
    free(raw);
    return buf;
}

static void load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    size_t len;
    if ((g_names_text = slurp(NAMES_PATH, &len))) {
        int lines = 0;
        for (size_t i = 0; i < len; i++) lines += g_names_text[i] == '\n';
        g_ent = malloc(sizeof *g_ent * (size_t)(lines + 1));
        for (char *p = g_names_text; g_ent && *p;) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            const char *e;
            uint32_t cp;
            if (hex(p, &e, &cp) && *e == ';') {
                g_ent[g_count].cp = cp;
                g_ent[g_count].name = e + 1;
                g_count++;
            }
            if (!nl) break;
            p = nl + 1;
        }
    }
    if ((g_blocks_text = slurp(BLOCKS_PATH, &len))) {
        int lines = 0;
        for (size_t i = 0; i < len; i++) lines += g_blocks_text[i] == '\n';
        g_blocks = malloc(sizeof *g_blocks * (size_t)(lines + 1));
        for (char *p = g_blocks_text; g_blocks && *p;) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            const char *e, *f;
            uint32_t lo, hi;
            if (hex(p, &e, &lo) && *e == ';' && hex(e + 1, &f, &hi) && *f == ';') {
                g_blocks[g_nblocks].lo = lo;
                g_blocks[g_nblocks].hi = hi;
                g_blocks[g_nblocks].name = f + 1;
                g_nblocks++;
            }
            if (!nl) break;
            p = nl + 1;
        }
    }
}

static int find(uint32_t cp) {
    int lo = 0, hi = g_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_ent[mid].cp == cp) return mid;
        if (g_ent[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

const char *uunicode_name(uint32_t cp) {
    load();
    int i = find(cp);
    return i >= 0 ? g_ent[i].name : 0;
}

int uunicode_named_count(void) { load(); return g_count; }
uint32_t uunicode_named(int i) { load(); return i >= 0 && i < g_count ? g_ent[i].cp : 0; }

int uunicode_block_count(void) { load(); return g_nblocks; }

const struct uunicode_block *uunicode_block(int i) {
    load();
    return i >= 0 && i < g_nblocks ? &g_blocks[i] : 0;
}

int uunicode_block_of(uint32_t cp) {
    load();
    for (int i = 0; i < g_nblocks; i++)
        if (cp >= g_blocks[i].lo && cp <= g_blocks[i].hi) return i;
    return -1;
}

// Does `name` hold `word` (upper case, `n` bytes) as a substring?
// Does a word of `name` START with `word`? A prefix, so a search narrows
// as it is typed ("arr" already finds the arrows); at a word's start, so
// "e" is not every name with an E in it.
static int has_word(const char *name, const char *word, int n) {
    for (const char *p = name; *p; p++)
        if ((p == name || p[-1] == ' ' || p[-1] == '-') && !strncmp(p, word, (size_t)n)) return 1;
    return 0;
}

int uunicode_search(const char *q, uint32_t *out, int cap) {
    load();
    while (*q == ' ') q++;
    if (!*q) return 0;
    // A code point, written as U+XXXX or as bare hex of 4 or more digits.
    const char *h = q;
    if ((h[0] == 'U' || h[0] == 'u') && h[1] == '+') h += 2;
    const char *e;
    uint32_t cp;
    int nd = hex(h, &e, &cp);
    if (nd && !*e && (h != q || nd >= 4)) {
        if (cap > 0) out[0] = cp;
        return 1;
    }
    // One character, given as itself: in UTF-8, or as the one Latin-1
    // byte a text field here holds (docs/decisions/drivers.md).
    int len;
    uint32_t c = uunicode_utf8_decode(q, &len);
    if (c == 0xFFFD && len == 1 && (unsigned char)q[0] >= 0xA0 && !q[1]) c = (unsigned char)q[0];
    if (c >= 0x80 && c != 0xFFFD && !q[len]) {
        if (cap > 0) out[0] = c;
        return 1;
    }
    // Words, all of them, case ignored: names are upper case.
    char words[8][32];
    int nw = 0;
    for (const char *p = q; *p && nw < 8;) {
        while (*p == ' ') p++;
        int n = 0;
        while (*p && *p != ' ') {
            if (n < 31) words[nw][n++] = (char)toupper((unsigned char)*p);
            p++;
        }
        words[nw][n] = '\0';
        if (n) nw++;
    }
    int found = 0;
    for (int i = 0; i < g_count; i++) {
        int all = 1;
        for (int w = 0; w < nw && all; w++) all = has_word(g_ent[i].name, words[w], (int)strlen(words[w]));
        if (!all) continue;
        if (found < cap) out[found] = g_ent[i].cp;
        found++;
    }
    return found;
}

int uunicode_utf8(uint32_t cp, char out[5]) {
    int n;
    if (cp < 0x80) { out[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp >= 0xD800 && cp <= 0xDFFF) n = 0;
    else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12); out[1] = (char)(0x80 | (cp >> 6 & 63));
        out[2] = (char)(0x80 | (cp & 63)); n = 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | cp >> 18); out[1] = (char)(0x80 | (cp >> 12 & 63));
        out[2] = (char)(0x80 | (cp >> 6 & 63)); out[3] = (char)(0x80 | (cp & 63)); n = 4;
    } else n = 0;
    out[n] = '\0';
    return n;
}

uint32_t uunicode_utf8_decode(const char *s, int *len) {
    const unsigned char *u = (const unsigned char *)s;
    int n = u[0] < 0x80 ? 1 : (u[0] & 0xE0) == 0xC0 ? 2 : (u[0] & 0xF0) == 0xE0 ? 3 : (u[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (!n) { *len = 1; return 0xFFFD; }
    uint32_t cp = n == 1 ? u[0] : (uint32_t)(u[0] & (0x7F >> n));
    for (int i = 1; i < n; i++) {
        if ((u[i] & 0xC0) != 0x80) { *len = 1; return 0xFFFD; }
        cp = cp << 6 | (u[i] & 63);
    }
    *len = n;
    return cp;
}
