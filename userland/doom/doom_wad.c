// The game data DOOM can run: which IWADs it knows, where they come
// from, which one is on this machine, and its title picture. See
// doom_internal.h.
#include "doom_internal.h"
#include "lib/uconf.h"
#include "lib/ubytes.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// **A DOWNLOAD IS PINNED BY ITS SHA-256**, Debian's game-data-packager
// shape: the checksum, not the transport, is what says the bytes are
// the game -- which is why the fetch may skip certificate checks on a
// machine with no CA bundle (uhttp.h's `insecure`), and why a mirror
// (`mirror=` in /etc/doom.conf) is as good as the origin.
//
// Freedoom's checksum is the one its release signs (freedoom-0.13.0-
// CHECKSUM, PGP-signed by the project); the shareware one is id's 1.9
// doom1.wad (MD5 f0cefca49926d00903cf57551d901abe, the widely published
// value). ORDER IS PREFERENCE when nothing is chosen in /etc/doom.conf:
// id's own data first, as before this list existed.
const struct doom_iwad DOOM_IWADS[] = {
    { "doom1.wad", "DOOM Shareware", "Episode 1 by id Software. Free to share unchanged.",
      "https://raw.githubusercontent.com/Akbar30Bill/DOOM_wads/master/doom1.wad",
      "1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771",
      4196020, NULL, "raw.githubusercontent.com",
      "id Software shareware licence. Not open source.",
      "id Software lets anyone share the shareware episode unchanged and free of "
      "charge. You may not sell it or change it, and it is not the full game.\n"
      "This copy comes from a third party's GitHub repository, not from id "
      "Software. DOOM checks that it is id's unmodified version 1.9 before using it.",
      1, "doom-shareware.txt", NULL },
    { "doom.wad", "DOOM", "The registered game.", NULL, NULL, 0, NULL, NULL, NULL, NULL, 0, NULL, NULL },
    { "doom2.wad", "DOOM II", "Hell on Earth.", NULL, NULL, 0, NULL, NULL, NULL, NULL, 0, NULL, NULL },
    { "freedoom1.wad", "Freedoom: Phase 1", "Free game content, BSD licence. Four episodes.",
      "https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip",
      "3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59",
      24143781, "freedoom-0.13.0/freedoom1.wad", "github.com/freedoom",
      "BSD 3-clause. Free software.",
      "Freedoom is free game content: you may use, share and change it, as long "
      "as its copyright notice stays with it.\n"
      "It comes from the Freedoom project's own releases on GitHub. DOOM checks "
      "the release's signed checksum before using it.",
      0, "freedoom.txt", "freedoom-0.13.0/COPYING.txt" },
    { "freedoom2.wad", "Freedoom: Phase 2", "Free game content, BSD licence.", NULL, NULL, 0, NULL, NULL,
      NULL, NULL, 0, NULL, NULL },
};
const int DOOM_IWAD_COUNT = (int)(sizeof DOOM_IWADS / sizeof DOOM_IWADS[0]);

int doom_iwad_path(const struct doom_iwad *w, char *out, int cap) {
    return snprintf(out, (size_t)cap, "%s/%s", DOOM_WAD_DIR, w->file) < cap;
}

int doom_iwad_present(const struct doom_iwad *w) {
    char p[160];
    return doom_iwad_path(w, p, sizeof p) && access(p, F_OK) == 0;
}

// CASE-BLIND: a retail DOOM.WAD off a DOS install or a FAT stick is the
// same file. The table's lower-case name is what it is installed as.
static int same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b - 'A' + 'a') : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

const struct doom_iwad *doom_iwad_named(const char *file) {
    for (int i = 0; i < DOOM_IWAD_COUNT; i++)
        if (same_name(DOOM_IWADS[i].file, file)) return &DOOM_IWADS[i];
    return NULL;
}

const struct doom_iwad *doom_iwad_chosen(void) {
    char v[32];
    if (uconf_get(DOOM_CONF, "iwad", v, sizeof v)) {
        const struct doom_iwad *w = doom_iwad_named(v);
        if (w && doom_iwad_present(w)) return w;
    }
    for (int i = 0; i < DOOM_IWAD_COUNT; i++)
        if (doom_iwad_present(&DOOM_IWADS[i])) return &DOOM_IWADS[i];
    return NULL;
}

int doom_iwad_choose(const struct doom_iwad *w) {
    return uconf_set(DOOM_CONF, "iwad", w->file);
}

int doom_iwad_url(const struct doom_iwad *w, char *out, int cap) {
    char mirror[128];
    if (!w->url) return 0;
    if (uconf_get(DOOM_CONF, "mirror", mirror, sizeof mirror) && mirror[0]) {
        const char *base = strrchr(w->url, '/');
        return snprintf(out, (size_t)cap, "%s%s", mirror, base) < cap;
    }
    return snprintf(out, (size_t)cap, "%s", w->url) < cap;
}

// --- the WAD itself --------------------------------------------------------

int doom_wad_is_iwad(const char *path) {
    unsigned char h[12];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    int ok = ub_read_at(fd, 0, h, sizeof h) == 0 && !memcmp(h, "IWAD", 4) && ub_le32(h + 4) > 0;
    close(fd);
    return ok;
}

#define LUMPS_MAX   65536
#define PATCH_MAX   (256u * 1024u)

// One lump by name, into a fresh allocation. NULL if absent or unreadable.
static unsigned char *lump(int fd, const unsigned char *dir, uint32_t n, const char *name,
                           uint32_t cap, uint32_t *len) {
    for (uint32_t i = 0; i < n; i++) {
        const unsigned char *e = dir + i * 16;
        if (strncmp((const char *)e + 8, name, 8)) continue;
        uint32_t pos = ub_le32(e), size = ub_le32(e + 4);
        if (size == 0 || size > cap) return NULL;
        unsigned char *b = malloc(size);
        if (b && ub_read_at(fd, pos, b, size) == 0) { *len = size; return b; }
        free(b);
        return NULL;
    }
    return NULL;
}

// TITLEPIC through PLAYPAL's first palette: a "patch" is columns of
// posts (APPNOTE's cousin, the Doom wiki's "Picture format"), every
// offset checked against the lump.
int doom_wad_titlepic(const char *path, uint32_t *out) {
    for (int i = 0; i < DOOM_TITLE_W * DOOM_TITLE_H; i++) out[i] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    unsigned char h[12];
    unsigned char *dir = NULL, *pal = NULL, *pic = NULL;
    int rc = -1;
    if (ub_read_at(fd, 0, h, sizeof h) || (memcmp(h, "IWAD", 4) && memcmp(h, "PWAD", 4))) goto out;
    uint32_t n = ub_le32(h + 4), at = ub_le32(h + 8);
    if (n == 0 || n > LUMPS_MAX) goto out;
    dir = malloc(n * 16);
    if (!dir || ub_read_at(fd, at, dir, n * 16)) goto out;
    uint32_t pl = 0, len = 0;
    pal = lump(fd, dir, n, "PLAYPAL", 64u * 1024u, &pl);
    pic = lump(fd, dir, n, "TITLEPIC", PATCH_MAX, &len);
    if (!pal || pl < 768 || !pic || len < 8) goto out;
    unsigned w = ub_le16(pic), ht = ub_le16(pic + 2);
    if (w == 0 || 8u + 4u * w > len) goto out;
    for (unsigned x = 0; x < w && x < DOOM_TITLE_W; x++) {
        uint32_t p = ub_le32(pic + 8 + 4 * x);
        while (p < len && pic[p] != 0xFF) {
            if (p + 3 > len) goto out;
            unsigned top = pic[p], cnt = pic[p + 1];
            if (p + 3 + cnt > len) goto out;
            for (unsigned k = 0; k < cnt; k++) {
                unsigned y = top + k;
                if (y >= ht || y >= DOOM_TITLE_H) continue;
                const unsigned char *c = pal + 3 * pic[p + 3 + k];
                out[y * DOOM_TITLE_W + x] = (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
            }
            p += 4 + cnt;
        }
    }
    rc = 0;
out:
    free(dir);
    free(pal);
    free(pic);
    close(fd);
    return rc;
}
