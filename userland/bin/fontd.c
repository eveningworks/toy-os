// fontd -- the session font, parsed and rasterized in RING 3.
//
// A `.ttf` is attacker-shaped data, and parsing it was ring-0 work here
// until this existed: the surface Windows spent a decade of GDI CVEs on
// before Windows 10 moved it to a sandboxed user-mode host. This is that
// host. A malformed font now crashes a restartable service instead of
// the kernel.
//
// WHAT IT DOES. Reads `system.font_face` and `system.font_size`, loads
// the face from /usr/share/fonts, rasterizes both weights with
// api/font_atlas.h -- the same implementation the kernel used to run --
// and publishes each as a public shm object every client maps read-only
// (abi/font_shm.h). Then it watches the settings and republishes when
// they move.
//
// WHAT IT DELIBERATELY DOES NOT DO. It does not serve the KERNEL. Ring 0
// draws its console, its shell and its panics from the baked tables
// compiled into the image, and must: those have to work before any
// process exists, and a console that cannot draw until a service starts
// is a console that cannot report why the service did not start.
//
// See docs/commands/fontd.md.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "ttf.h"
#include "font_atlas.h"
#include "font_face.h"   // FONT_WEIGHT_*, FONT_BOLD_SUFFIX
#include "font_shm.h"
#include "diag_abi.h"
#include "syscall_abi.h"   // SHM_*, SYS_PROT_*, SYS_MAP_SHARED
#include "lib/usetting.h"
#include "lib/ufile.h"

#define FONT_DIR   "/usr/share/fonts/"
#define POLL_MS    250
#define FACE_MAX   32

static char g_face[FACE_MAX];
static int  g_px;
static uint32_t g_generation;

// One weight's object. Kept open so a republish can unlink the old name
// before creating the new one -- an object somebody still has mapped
// survives its own unlink, which is what stops a client faulting
// mid-frame while the font changes under it.
struct published {
    int fd;
    void *base;
    size_t bytes;
};
static struct published g_pub[FONT_WEIGHT_COUNT];

// The beacon: one page, created once, written IN PLACE. It is how a
// client notices a republish at all -- see abi/font_shm.h on why the
// atlas objects themselves cannot say.
static struct font_beacon *g_beacon;

static void beacon_init(void) {
    sys_shm_unlink(FONT_BEACON_NAME);
    int fd = sys_shm_open(FONT_BEACON_NAME, sizeof(struct font_beacon),
                          SHM_CREATE | SHM_EXCL | SHM_PUBLIC);
    if (fd < 0) { printf("fontd: cannot create the beacon (%d)\n", fd); return; }
    void *p = sys_mmap(0, sizeof(struct font_beacon),
                       SYS_PROT_READ | SYS_PROT_WRITE, SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    if (!p) { printf("fontd: cannot map the beacon\n"); return; }
    g_beacon = (struct font_beacon *)p;
    g_beacon->generation = 0;
    g_beacon->magic = FONT_BEACON_MAGIC;
}

// AFTER both weights, never between them: a client that re-opened on a
// half-done republish would take the new regular and the old bold, and
// draw a heading in the wrong face.
static void beacon_bump(void) {
    if (g_beacon) g_beacon->generation = g_generation;
}

static void unpublish(int weight) {
    struct published *p = &g_pub[weight];
    if (p->base) sys_munmap(p->base, p->bytes);
    if (p->fd >= 0) sys_close(p->fd);
    p->base = NULL;
    p->fd = -1;
    p->bytes = 0;
}

// Rasterizes `weight` of the current face and publishes it. Returns 1 on
// success; a failure leaves the PREVIOUS object in place, because a
// client drawing yesterday's font is better than one drawing none.
static int publish(int weight) {
    char path[128];
    uint8_t *bytes = NULL;
    size_t len = 0;
    int synthesizing = 0;

    if (weight == FONT_WEIGHT_BOLD) {
        snprintf(path, sizeof path, "%s%s%s.ttf", FONT_DIR, g_face, FONT_BOLD_SUFFIX);
        // A FAMILY WITH NO BOLD FILE gets its regular outlines smeared,
        // which is what GDI, Cairo and DirectWrite all fall back to. A
        // client cannot tell and does not need to.
        if (ufile_slurp(path, 0, &bytes, &len) != UFILE_OK) synthesizing = 1;
    }
    if (!bytes) {
        snprintf(path, sizeof path, "%s%s.ttf", FONT_DIR, g_face);
        if (ufile_slurp(path, 0, &bytes, &len) != UFILE_OK) {
            printf("fontd: cannot read %s\n", path);
            return 0;
        }
    }

    struct ttf_font t;
    if (!ttf_open(&t, bytes, len)) {
        printf("fontd: %s is not a TrueType outline font\n", path);
        free(bytes);
        return 0;
    }

    struct font_atlas_plan plan;
    if (!font_atlas_plan(&t, g_px, weight, synthesizing, &plan)) {
        printf("fontd: %s refused at %dpx\n", g_face, g_px);
        free(bytes);
        return 0;
    }

    size_t total = sizeof(struct font_shm) + (size_t)plan.total_bytes;
    char name[SHM_NAME_MAX];
    snprintf(name, sizeof name, FONT_SHM_NAME_FMT, weight);

    // UNLINK THEN CREATE, never reuse: a client holding the old object
    // keeps reading it until it re-opens, and writing a new atlas into
    // memory somebody is drawing from would tear every glyph on screen.
    sys_shm_unlink(name);
    int fd = sys_shm_open(name, total, SHM_CREATE | SHM_EXCL | SHM_PUBLIC);
    if (fd < 0) {
        printf("fontd: cannot create %s (%d)\n", name, fd);
        free(bytes);
        return 0;
    }
    void *base = sys_mmap(0, total, SYS_PROT_READ | SYS_PROT_WRITE,
                          SYS_MAP_SHARED, fd, 0);
    if (!base) {
        printf("fontd: cannot map %s\n", name);
        sys_close(fd);
        sys_shm_unlink(name);
        free(bytes);
        return 0;
    }

    struct font_shm *h = (struct font_shm *)base;
    memset(h, 0, sizeof *h);
    uint8_t *blob = (uint8_t *)base + sizeof *h;

    struct ttf_scratch *sc = (struct ttf_scratch *)malloc(sizeof *sc);
    if (!sc) {
        printf("fontd: no memory for scratch\n");
        sys_munmap(base, total);
        sys_close(fd);
        sys_shm_unlink(name);
        free(bytes);
        return 0;
    }
    memset(blob, 0, (size_t)plan.total_bytes);
    font_atlas_render(&t, &plan, blob, sc);
    free(sc);
    free(bytes);

    h->version   = FONT_SHM_VERSION;
    h->px        = (uint32_t)plan.px;
    h->weight    = (uint32_t)weight;
    h->cell_w    = (uint32_t)plan.cell_w;
    h->cell_h    = (uint32_t)plan.cell_h;
    h->line_h    = (uint32_t)plan.line_h;
    h->baseline  = (uint32_t)plan.baseline;
    h->count     = (uint32_t)plan.count;
    h->monospace = (uint32_t)plan.monospace;
    h->synthetic = (uint32_t)plan.synthetic;
    h->glyph_off = (uint32_t)sizeof *h;
    h->adv_off   = (uint32_t)(sizeof *h + plan.glyph_bytes);
    h->kern_off  = (uint32_t)(sizeof *h + plan.glyph_bytes + (uint64_t)plan.count);
    h->bytes     = (uint32_t)total;
    strlcpy(h->face, g_face, sizeof h->face);

    // THE MAGIC AND THE GENERATION GO LAST, in that order. A client that
    // maps this while it is being filled must see either "not ready" or a
    // complete atlas, never a half-written one -- so nothing else is
    // published until every byte above is in place.
    h->generation = ++g_generation;
    h->magic = FONT_SHM_MAGIC;

    unpublish(weight);
    g_pub[weight].fd = fd;
    g_pub[weight].base = base;
    g_pub[weight].bytes = total;

    printf("fontd: %s %s at %dpx -- %dx%d cell (line %d), %d glyphs, %s, gen %u\n",
          g_face, weight == FONT_WEIGHT_BOLD ? "bold" : "regular", plan.px,
          plan.cell_w, plan.cell_h, plan.line_h, plan.count,
          plan.monospace ? "monospace" : "proportional", h->generation);
    return 1;
}

// The settings, as they stand. Returns 1 when either has moved.
static int settings_changed(void) {
    char face[FACE_MAX] = {0};
    int px = 0;

    if (usetting_get("system.font_face", face, sizeof face) <= 0) return 0;
    if (usetting_get_int("system.font_size", &px) < 0 || px <= 0) return 0;

    // `builtin` is the sentinel for the baked tables. There is nothing
    // to rasterize for it, so fontd publishes nothing and every client
    // falls back to what the kernel already hands out.
    if (strcmp(face, "builtin") == 0) {
        if (g_face[0]) {
            printf("fontd: face is `builtin' -- unpublishing\n");
            for (int w = 0; w < FONT_WEIGHT_COUNT; w++) {
                char name[SHM_NAME_MAX];
                snprintf(name, sizeof name, FONT_SHM_NAME_FMT, w);
                sys_shm_unlink(name);
                unpublish(w);
            }
            g_face[0] = '\0';
            g_generation++;
            beacon_bump();
        }
        return 0;
    }

    if (strcmp(face, g_face) == 0 && px == g_px) return 0;
    strlcpy(g_face, face, sizeof g_face);
    g_px = px;
    return 1;
}

// `diag font` -- what is published, for somebody asking why their text
// looks wrong. Registered under the name `font`; see abi/diag_abi.h.
static void answer_diag(void) {
    // STATIC: a diag_msg plus the reply buffer is most of the ring-3
    // frame budget (userland/rt/link.ld), and this is called every tick.
    static struct diag_msg q;
    memset(&q, 0, sizeof q);
    q.type = DIAG_TAKE;
    if (sys_diag(&q) != 1) return;

    static char out[DIAG_CHUNK + 1];
    int n = 0;
    if (!g_face[0]) {
        n = snprintf(out, sizeof out,
                     "no face published -- the baked tables are in use\n");
    } else {
        n = snprintf(out, sizeof out, "face %s at %dpx, generation %u\n",
                     g_face, g_px, g_generation);
        for (int w = 0; w < FONT_WEIGHT_COUNT && n < (int)sizeof out - 80; w++) {
            const struct font_shm *h = (const struct font_shm *)g_pub[w].base;
            if (!h) continue;
            n += snprintf(out + n, sizeof out - (size_t)n,
                          "  %-8s %ux%u cell, line %u, %u glyphs, %s%s\n",
                          w == FONT_WEIGHT_BOLD ? "bold" : "regular",
                          h->cell_w, h->cell_h, h->line_h, h->count,
                          h->monospace ? "monospace" : "proportional",
                          h->synthetic ? ", synthesized" : "");
        }
    }

    static struct diag_msg r;
    memset(&r, 0, sizeof r);
    r.type = DIAG_REPLY;
    if (n < 0) n = 0;
    if (n > DIAG_CHUNK) n = DIAG_CHUNK;
    memcpy(r.text, out, (size_t)n);
    r.text[n] = '\0';
    r.len = (uint32_t)n;
    sys_diag(&r);
}

int main(void) {
    for (int w = 0; w < FONT_WEIGHT_COUNT; w++) { g_pub[w].fd = -1; }

    // The diagnostic name, claimed once. A service that cannot be asked
    // what it is doing is one whose failures get guessed at.
    static struct diag_msg c;
    memset(&c, 0, sizeof c);
    c.type = DIAG_CLAIM;
    strlcpy(c.name, "font", DIAG_NAME_LEN);
    sys_diag(&c);

    beacon_init();
    if (settings_changed()) {
        publish(FONT_WEIGHT_REGULAR);
        publish(FONT_WEIGHT_BOLD);
        beacon_bump();
    }

    // READY EVEN WITH NOTHING PUBLISHED. A machine whose face is
    // `builtin`, or whose font file is missing, still has a working
    // desktop on the baked tables -- so refusing to come up would take
    // the session down over a cosmetic problem.
    sys_notify_ready();

    for (;;) {
        if (settings_changed()) {
            publish(FONT_WEIGHT_REGULAR);
            publish(FONT_WEIGHT_BOLD);
            beacon_bump();
        }
        answer_diag();
        sys_sleep_ms(POLL_MS);
    }
    return 0;
}
