// A LIT SOLID, TUMBLING. Windows' Flying Objects, and the saver that
// shows off something toy-os actually has.
//
// EVERY PIXEL COMES FROM THE SHARED GEOMETRY MODULE (api/geom.h), the
// same `geom.c` the kernel links -- `geom_transform3` for the tumble
// and the projection, `geom_face_normal3` for which way a face points,
// `geom_shade` for how lit it is. Q16.16 throughout, and **angles are
// in TURNS**: FX_ONE is a full rotation, so a tumble is three counters
// that wrap by masking rather than by a modulo against pi.
//
// NO DEPTH BUFFER, because geom.h is deliberately not a 3D engine: the
// faces are sorted back-to-front by their rotated depth and painted in
// that order, which is the painter's algorithm and is exact for a
// convex solid. An octahedron is convex, which is why it is this one.
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "geom.h"
#include "fixed.h"

#define V(a, b, c) { (a) * FX_ONE, (b) * FX_ONE, (c) * FX_ONE }

// Six vertices, eight triangular faces, every face wound so its normal
// points OUT of the solid -- which is what makes the front-facing test
// below a sign test and nothing more (see geom_face_normal3).
static const struct geom_pt3 VERT[6] = {
    V(0, -1, 0), V(1, 0, 0), V(0, 0, 1), V(-1, 0, 0), V(0, 0, -1), V(0, 1, 0),
};
static const int FACE[8][3] = {
    {0,1,2}, {0,2,3}, {0,3,4}, {0,4,1},
    {5,2,1}, {5,3,2}, {5,4,3}, {5,1,4},
};

// The face colours, so adjacent faces differ even when equally lit --
// a solid shaded only by angle reads as a blob when two faces catch the
// light the same way.
static const uint32_t FACE_RGB[8] = {
    0x4080FF, 0x40C0FF, 0x40FFC0, 0x80FF40,
    0xFFC040, 0xFF8040, 0xFF4080, 0xC040FF,
};

static fx_t g_yaw, g_pitch, g_roll;

static void plot(void *ctx, int x, int y, uint32_t color, uint8_t alpha) {
    (void)alpha;
    ugfx_put_pixel((struct ugfx_surface *)ctx, x, y, color);
}

static uint32_t shade_rgb(uint32_t rgb, int lit) {
    // 64 of ambient, so a face turned away is dark rather than absent:
    // an unlit face painted black is a hole in the solid.
    int l = 64 + (lit * 191) / 255;
    unsigned r = ((rgb >> 16) & 0xFF) * (unsigned)l / 255;
    unsigned g = ((rgb >> 8) & 0xFF) * (unsigned)l / 255;
    unsigned b = (rgb & 0xFF) * (unsigned)l / 255;
    return (r << 16) | (g << 8) | b;
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = d->surface;
    ugfx_fill_rect(s, 0, 0, s->w, s->h, 0x000000);
    struct geom_target t = { plot, s };

    int xs[6], ys[6];
    fx_t zs[6];
    // THE EYE DISTANCE IS IN THE SCALED MODEL'S UNITS, which is the one
    // thing easy to get wrong here: the vertices above are +-1.0, `scale`
    // turns that into a pixel radius, and `dist` has to be in the SAME
    // units. A radius of 240 with a distance of 8 puts the eye inside
    // the solid, the projection clamps, and the nearest face fills the
    // screen with one flat colour -- which is exactly what it did.
    fx_t r = fx_from_int((s->h < s->w ? s->h : s->w) / 3);
    geom_transform3(VERT, 6, g_yaw, g_pitch, g_roll, r,
                    r * 4, s->w / 2, s->h / 2, xs, ys, zs);

    // BACK TO FRONT. Eight faces, so an insertion sort over their mean
    // depth is the whole of it -- and a sort is what a convex solid
    // needs instead of a depth buffer.
    int order[8];
    fx_t depth[8];
    for (int f = 0; f < 8; f++) {
        order[f] = f;
        depth[f] = (zs[FACE[f][0]] + zs[FACE[f][1]] + zs[FACE[f][2]]) / 3;
    }
    for (int i = 1; i < 8; i++) {
        int k = order[i];
        fx_t dk = depth[k];
        int j = i - 1;
        while (j >= 0 && depth[order[j]] < dk) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }

    // The light comes over the viewer's shoulder, slightly to the left.
    struct geom_pt3 light = { -FX_ONE / 3, -FX_ONE / 3, -FX_ONE };
    for (int i = 0; i < 8; i++) {
        const int *f = FACE[order[i]];
        struct geom_pt3 ra = geom_rotate3(VERT[f[0]], g_yaw, g_pitch, g_roll);
        struct geom_pt3 rb = geom_rotate3(VERT[f[1]], g_yaw, g_pitch, g_roll);
        struct geom_pt3 rc = geom_rotate3(VERT[f[2]], g_yaw, g_pitch, g_roll);
        struct geom_pt3 n = geom_face_normal3(ra, rb, rc);
        if (n.z > 0) continue;   // wound away from the eye -- a back face
        int px[3] = { xs[f[0]], xs[f[1]], xs[f[2]] };
        int py[3] = { ys[f[0]], ys[f[1]], ys[f[2]] };
        geom_fill_polygon(&t, px, py, 3,
                          shade_rgb(FACE_RGB[order[i]], geom_shade(n, light)));
    }
}

static int on_tick(struct uapp *a) {
    (void)a;
    // THREE DIFFERENT RATES, so the tumble never repeats on a short
    // cycle -- equal rates give a solid that rocks back and forth.
    g_yaw   = (g_yaw   + FX_ONE / 420) & (FX_ONE - 1);
    g_pitch = (g_pitch + FX_ONE / 260) & (FX_ONE - 1);
    g_roll  = (g_roll  + FX_ONE / 730) & (FX_ONE - 1);
    return 1;
}

static void on_open(struct uapp *a) { uapp_set_fullscreen(a, 1); }

int main(void) {
    struct uapp_desc desc = {
        .title = "Solid",
        .app_id = "saver-solid",
        .flags = UAPP_RESIZABLE,
        .w = 640, .h = 480,
        .tick_ms = 33,
        .on_open = on_open,
        .on_tick = on_tick,
        .on_draw = on_draw,
    };
    return uapp_run(&desc);
}
