// The Utah teapot, tessellated -- see teapot.h for what comes out.
//
// THE DATA is Martin Newell's 1975 patches (Crow, "The Origins of the
// Teapot", IEEE CG&A, 1987), in the MIRRORED form freeglut ships: ten
// patches over 129 control points, each of the first six (rim, body,
// lid, bottom) used four times about the x and y planes and the last
// four (handle, spout) twice about y -- 6 x 4 + 4 x 2 = the 32. Newell's
// frame is z UP; build() turns it into geom.h's y-down one.
//
// Coordinates are in 1/100000 of Newell's units, which is exact for all
// of them (1.3375, 2.53125) and converted to Q16.16 once.
#include "shapes/teapot.h"

static const int32_t CP[129][3] = {
    { 140000, 0, 240000 }, { 140000, -78400, 240000 }, { 78400, -140000, 240000 },
    { 0, -140000, 240000 }, { 133750, 0, 253125 }, { 133750, -74900, 253125 },
    { 74900, -133750, 253125 }, { 0, -133750, 253125 }, { 143750, 0, 253125 },
    { 143750, -80500, 253125 }, { 80500, -143750, 253125 }, { 0, -143750, 253125 },
    { 150000, 0, 240000 }, { 150000, -84000, 240000 }, { 84000, -150000, 240000 },
    { 0, -150000, 240000 }, { 175000, 0, 187500 }, { 175000, -98000, 187500 },
    { 98000, -175000, 187500 }, { 0, -175000, 187500 }, { 200000, 0, 135000 },
    { 200000, -112000, 135000 }, { 112000, -200000, 135000 }, { 0, -200000, 135000 },
    { 200000, 0, 90000 }, { 200000, -112000, 90000 }, { 112000, -200000, 90000 },
    { 0, -200000, 90000 }, { 200000, 0, 45000 }, { 200000, -112000, 45000 },
    { 112000, -200000, 45000 }, { 0, -200000, 45000 }, { 150000, 0, 22500 },
    { 150000, -84000, 22500 }, { 84000, -150000, 22500 }, { 0, -150000, 22500 },
    { 150000, 0, 15000 }, { 150000, -84000, 15000 }, { 84000, -150000, 15000 },
    { 0, -150000, 15000 }, { 0, 0, 315000 }, { 0, -200, 315000 },
    { 200, 0, 315000 }, { 80000, 0, 315000 }, { 80000, -45000, 315000 },
    { 45000, -80000, 315000 }, { 0, -80000, 315000 }, { 0, 0, 285000 },
    { 20000, 0, 270000 }, { 20000, -11200, 270000 }, { 11200, -20000, 270000 },
    { 0, -20000, 270000 }, { 40000, 0, 255000 }, { 40000, -22400, 255000 },
    { 22400, -40000, 255000 }, { 0, -40000, 255000 }, { 130000, 0, 255000 },
    { 130000, -72800, 255000 }, { 72800, -130000, 255000 }, { 0, -130000, 255000 },
    { 130000, 0, 240000 }, { 130000, -72800, 240000 }, { 72800, -130000, 240000 },
    { 0, -130000, 240000 }, { 0, 0, 0 }, { 0, -142500, 0 },
    { 79800, -142500, 0 }, { 142500, -79800, 0 }, { 142500, 0, 0 },
    { 0, -150000, 7500 }, { 84000, -150000, 7500 }, { 150000, -84000, 7500 },
    { 150000, 0, 7500 }, { -160000, 0, 202500 }, { -160000, -30000, 202500 },
    { -150000, -30000, 225000 }, { -150000, 0, 225000 }, { -230000, 0, 202500 },
    { -230000, -30000, 202500 }, { -250000, -30000, 225000 }, { -250000, 0, 225000 },
    { -270000, 0, 202500 }, { -270000, -30000, 202500 }, { -300000, -30000, 225000 },
    { -300000, 0, 225000 }, { -270000, 0, 180000 }, { -270000, -30000, 180000 },
    { -300000, -30000, 180000 }, { -300000, 0, 180000 }, { -270000, 0, 157500 },
    { -270000, -30000, 157500 }, { -300000, -30000, 135000 }, { -300000, 0, 135000 },
    { -250000, 0, 112500 }, { -250000, -30000, 112500 }, { -265000, -30000, 93750 },
    { -265000, 0, 93750 }, { -200000, 0, 90000 }, { -200000, -30000, 90000 },
    { -190000, -30000, 60000 }, { -190000, 0, 60000 }, { 170000, 0, 142500 },
    { 170000, -66000, 142500 }, { 170000, -66000, 60000 }, { 170000, 0, 60000 },
    { 260000, 0, 142500 }, { 260000, -66000, 142500 }, { 310000, -66000, 82500 },
    { 310000, 0, 82500 }, { 230000, 0, 210000 }, { 230000, -25000, 210000 },
    { 240000, -25000, 202500 }, { 240000, 0, 202500 }, { 270000, 0, 240000 },
    { 270000, -25000, 240000 }, { 330000, -25000, 240000 }, { 330000, 0, 240000 },
    { 280000, 0, 247500 }, { 280000, -25000, 247500 }, { 352500, -25000, 249375 },
    { 352500, 0, 249375 }, { 290000, 0, 247500 }, { 290000, -15000, 247500 },
    { 345000, -15000, 251250 }, { 345000, 0, 251250 }, { 280000, 0, 240000 },
    { 280000, -15000, 240000 }, { 320000, -15000, 240000 }, { 320000, 0, 240000 },
};


static const uint8_t PATCH[10][16] = {
    {   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15 },
    {  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27 },
    {  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39 },
    {  40,  41,  42,  40,  43,  44,  45,  46,  47,  47,  47,  47,  48,  49,  50,  51 },
    {  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63 },
    {  64,  64,  64,  64,  65,  66,  67,  68,  69,  70,  71,  72,  39,  38,  37,  36 },
    {  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88 },
    {  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  98,  99, 100 },
    { 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116 },
    { 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128 },
};

static fx_t cp_fx(int32_t v) { return (fx_t)((int64_t)v * FX_ONE / 100000); }

// The cubic Bernstein basis at t, and its derivative.
static void basis(fx_t t, fx_t b[4], fx_t d[4]) {
    fx_t s = FX_ONE - t;
    fx_t ss = fx_mul(s, s), tt = fx_mul(t, t), st = fx_mul(s, t);
    b[0] = fx_mul(ss, s);
    b[1] = 3 * fx_mul(ss, t);
    b[2] = 3 * fx_mul(st, t);
    b[3] = fx_mul(tt, t);
    d[0] = -3 * ss;
    d[1] = 3 * ss - 6 * st;
    d[2] = 6 * st - 3 * tt;
    d[3] = 3 * tt;
}

// The point at (u, v) and the two tangents -- rows run along u, columns
// along v.
static void eval(const struct geom_pt3 P[16], fx_t u, fx_t v,
                 struct geom_pt3 *p, struct geom_pt3 *pu, struct geom_pt3 *pv) {
    fx_t bu[4], du[4], bv[4], dv[4];
    basis(u, bu, du);
    basis(v, bv, dv);
    int64_t a[3] = { 0, 0, 0 }, b[3] = { 0, 0, 0 }, c[3] = { 0, 0, 0 };
    for (int r = 0; r < 4; r++) {
        for (int k = 0; k < 4; k++) {
            const struct geom_pt3 *q = &P[r * 4 + k];
            fx_t w0 = fx_mul(bu[r], bv[k]), w1 = fx_mul(du[r], bv[k]), w2 = fx_mul(bu[r], dv[k]);
            a[0] += fx_mul(w0, q->x); a[1] += fx_mul(w0, q->y); a[2] += fx_mul(w0, q->z);
            b[0] += fx_mul(w1, q->x); b[1] += fx_mul(w1, q->y); b[2] += fx_mul(w1, q->z);
            c[0] += fx_mul(w2, q->x); c[1] += fx_mul(w2, q->y); c[2] += fx_mul(w2, q->z);
        }
    }
    *p  = (struct geom_pt3){ (fx_t)a[0], (fx_t)a[1], (fx_t)a[2] };
    *pu = (struct geom_pt3){ (fx_t)b[0], (fx_t)b[1], (fx_t)b[2] };
    *pv = (struct geom_pt3){ (fx_t)c[0], (fx_t)c[1], (fx_t)c[2] };
}

// pv x pu, which points OUT of Newell's surface, scaled so its largest
// component is +-1.0. 0 when the tangents are parallel or vanish.
static int normal(struct geom_pt3 pu, struct geom_pt3 pv, struct geom_pt3 *n) {
    int64_t x = (int64_t)pv.y * pu.z - (int64_t)pv.z * pu.y;
    int64_t y = (int64_t)pv.z * pu.x - (int64_t)pv.x * pu.z;
    int64_t z = (int64_t)pv.x * pu.y - (int64_t)pv.y * pu.x;
    int64_t m = x < 0 ? -x : x, ay = y < 0 ? -y : y, az = z < 0 ? -z : z;
    if (ay > m) m = ay;
    if (az > m) m = az;
    // Below ~1e-4 of a unit squared the direction is noise, not a normal.
    if (m < ((int64_t)FX_ONE * FX_ONE >> 13)) return 0;
    *n = (struct geom_pt3){ (fx_t)(x * FX_ONE / m), (fx_t)(y * FX_ONE / m),
                            (fx_t)(z * FX_ONE / m) };
    return 1;
}

// Newell's frame (x right, y back, z up) to geom's (x right, y down,
// z away): a proper rotation, so an outward normal stays outward.
static struct geom_pt3 to_view(struct geom_pt3 p) {
    return (struct geom_pt3){ p.x, -p.z, p.y };
}

void teapot_build(struct teapot *t) {
    int out = 0;
    for (int src = 0; src < 10; src++) {
        static const int8_t MIRROR[4][2] = { { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
        int copies = src < 6 ? 4 : 2;
        for (int m = 0; m < copies; m++) {
            // Handle and spout mirror about y only: MIRROR[0] and [2].
            int sx = MIRROR[src < 6 ? m : m * 2][0], sy = MIRROR[src < 6 ? m : m * 2][1];
            struct geom_pt3 P[16];
            for (int r = 0; r < 4; r++) {
                for (int k = 0; k < 4; k++) {
                    // A ONE-AXIS mirror reverses the winding; reading the
                    // columns backwards reverses it again.
                    int col = sx * sy < 0 ? 3 - k : k;
                    const int32_t *c = CP[PATCH[src][r * 4 + col]];
                    P[r * 4 + k] = (struct geom_pt3){ cp_fx(c[0] * sx), cp_fx(c[1] * sy),
                                                      cp_fx(c[2]) };
                }
            }
            for (int i = 0; i <= TEAPOT_DIV; i++) {
                for (int j = 0; j <= TEAPOT_DIV; j++) {
                    fx_t u = (fx_t)(i * FX_ONE / TEAPOT_DIV), v = (fx_t)(j * FX_ONE / TEAPOT_DIV);
                    struct geom_pt3 p, pu, pv, n = { 0, 0, -FX_ONE };
                    eval(P, u, v, &p, &pu, &pv);
                    if (!normal(pu, pv, &n)) {
                        // A pole -- the lid's knob, the bottom's centre --
                        // where a whole row of control points coincides
                        // and one tangent vanishes. The surface a hair
                        // inside has the direction the pole should have.
                        const fx_t e = FX_ONE / 64;
                        struct geom_pt3 q;
                        eval(P, u < e ? e : u > FX_ONE - e ? FX_ONE - e : u,
                             v < e ? e : v > FX_ONE - e ? FX_ONE - e : v, &q, &pu, &pv);
                        normal(pu, pv, &n);
                    }
                    int at = teapot_vert(out, i, j);
                    t->pos[at] = to_view(p);
                    t->nrm[at] = to_view(n);
                }
            }
            out++;
        }
    }

    // Centred on the bounding box, and the radius around that centre.
    fx_t lo[3] = { 0x7fffffff, 0x7fffffff, 0x7fffffff }, hi[3] = { -0x7fffffff, -0x7fffffff, -0x7fffffff };
    for (int i = 0; i < TEAPOT_VERTS; i++) {
        fx_t c[3] = { t->pos[i].x, t->pos[i].y, t->pos[i].z };
        for (int k = 0; k < 3; k++) {
            if (c[k] < lo[k]) lo[k] = c[k];
            if (c[k] > hi[k]) hi[k] = c[k];
        }
    }
    int64_t r2 = 0;
    for (int i = 0; i < TEAPOT_VERTS; i++) {
        t->pos[i].x -= (lo[0] + hi[0]) / 2;
        t->pos[i].y -= (lo[1] + hi[1]) / 2;
        t->pos[i].z -= (lo[2] + hi[2]) / 2;
        int64_t d = (int64_t)t->pos[i].x * t->pos[i].x + (int64_t)t->pos[i].y * t->pos[i].y +
                    (int64_t)t->pos[i].z * t->pos[i].z;
        if (d > r2) r2 = d;
    }
    // Integer square root of r2 (Q32.32) gives Q16.16 directly.
    uint64_t rem = (uint64_t)r2, root = 0, bit = 1ull << 62;
    while (bit > rem) bit >>= 2;
    while (bit) {
        if (rem >= root + bit) { rem -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    t->radius = (fx_t)root;
}
