// See usolid.h.
#include "lib/usolid.h"
#include <string.h>

// Model constants, Q16.16. The pyramid is a regular tetrahedron of edge
// 2.8 and the ball a sphere of radius 1.2 -- each sized to look about as
// big as the cube of half-edge 1 beside it, not to match it in any one
// measurement.
#define TET_A      183501   // edge
#define TET_HALF    91750   // edge / 2
#define TET_RC     105945   // the front face's circumradius, a / sqrt 3
#define TET_RI      52973   // ...and its inradius, a / (2 sqrt 3)
#define TET_RT      37457   // the solid's inradius, a / (2 sqrt 6): front face at -RT
#define TET_HF     158916   // a face's height, a sqrt 3 / 2
#define TET_CLOSED  19929   // 180 - 70.53 degrees (the dihedral), in turns
#define BALL_R      78643
#define BALL_W     494130   // 2 pi r: the flat sheet the ball rolls up from
#define BALL_H     247065   // pi r
#define BALL_LON 24
#define BALL_LAT 12

#define CUBE_CLOSED (FX_ONE / 4)

// Light from over the viewer's left shoulder; the eye is at -z.
static const struct geom_pt3 LIGHT = { -FX_ONE / 2, -FX_ONE * 2 / 3, -FX_ONE };

static struct geom_pt3 P(fx_t x, fx_t y, fx_t z) { struct geom_pt3 p = { x, y, z }; return p; }
static struct geom_pt3 add(struct geom_pt3 a, struct geom_pt3 b) { return P(a.x + b.x, a.y + b.y, a.z + b.z); }
static struct geom_pt3 scale(struct geom_pt3 a, fx_t k) { return P(fx_mul(a.x, k), fx_mul(a.y, k), fx_mul(a.z, k)); }
static struct geom_pt3 lerp3(struct geom_pt3 a, struct geom_pt3 b, fx_t t) {
    return P(a.x + fx_mul(b.x - a.x, t), a.y + fx_mul(b.y - a.y, t), a.z + fx_mul(b.z - a.z, t));
}

// A hinge's angle at progress `p`: from half a turn back -- the face lying
// over the front, wound away -- through flat, to `closed`.
static fx_t hinge(const fx_t *stage, int i, fx_t closed) {
    fx_t p = stage ? stage[i] : FX_ONE;
    if (p < 0) p = 0;
    if (p > FX_ONE) p = FX_ONE;
    return -FX_ONE / 2 + fx_mul(p, closed + FX_ONE / 2);
}

static int vert(struct usolid *s, struct geom_pt3 p, int u, int v) {
    int i = s->nv++;
    s->v[i].p = p;
    s->v[i].n = P(0, 0, 0);
    s->v[i].u = u;
    s->v[i].v = v;
    return i;
}

static void tri(struct usolid *s, int a, int b, int c) {
    s->t[s->nt][0] = (uint16_t)a;
    s->t[s->nt][1] = (uint16_t)b;
    s->t[s->nt][2] = (uint16_t)c;
    s->nt++;
}

// A face as seen from OUTSIDE: TL, TR, BR, BL, clockwise on screen, the
// picture upright. Every face is listed that way, so one test of screen
// winding culls the lot and no face shows its picture mirrored.
static void quad(struct usolid *s, struct geom_pt3 tl, struct geom_pt3 tr,
                 struct geom_pt3 br, struct geom_pt3 bl, int W, int H) {
    int a = vert(s, tl, 0, 0), b = vert(s, tr, W, 0);
    int c = vert(s, br, W, H), d = vert(s, bl, 0, H);
    tri(s, a, b, c);
    tri(s, a, c, d);
}

// --- cube ---------------------------------------------------------------
//
// The front face sits at z = -1 and each flap is a hinge on one of its
// edges: d is the flap's direction away from the hinge, (cos, sin) of the
// hinge angle in the plane it turns in, so 0 is flat and a quarter turn is
// folded back. The back face hinges on the right face's far edge, so its
// angle adds to the right face's.
static void build_cube(struct usolid *s, const fx_t *stage, int W, int H) {
    const fx_t O = FX_ONE, T = 2 * FX_ONE;
    quad(s, P(-O, -O, -O), P(O, -O, -O), P(O, O, -O), P(-O, O, -O), W, H);

    fx_t r = hinge(stage, 0, CUBE_CLOSED);
    struct geom_pt3 d = P(fx_cos(r), 0, fx_sin(r));
    struct geom_pt3 rt = P(O, -O, -O), rb = P(O, O, -O);
    struct geom_pt3 rt2 = add(rt, scale(d, T)), rb2 = add(rb, scale(d, T));
    quad(s, rt, rt2, rb2, rb, W, H);

    fx_t b = r + hinge(stage, 4, CUBE_CLOSED);
    d = P(fx_cos(b), 0, fx_sin(b));
    quad(s, rt2, add(rt2, scale(d, T)), add(rb2, scale(d, T)), rb2, W, H);

    fx_t l = hinge(stage, 1, CUBE_CLOSED);
    d = P(-fx_cos(l), 0, fx_sin(l));
    struct geom_pt3 lt = P(-O, -O, -O), lb = P(-O, O, -O);
    quad(s, add(lt, scale(d, T)), lt, lb, add(lb, scale(d, T)), W, H);

    fx_t t = hinge(stage, 2, CUBE_CLOSED);
    d = P(0, -fx_cos(t), fx_sin(t));
    struct geom_pt3 tl = P(-O, -O, -O), tr = P(O, -O, -O);
    quad(s, add(tl, scale(d, T)), add(tr, scale(d, T)), tr, tl, W, H);

    fx_t m = hinge(stage, 3, CUBE_CLOSED);
    d = P(0, fx_cos(m), fx_sin(m));
    struct geom_pt3 bl = P(-O, O, -O), br = P(O, O, -O);
    quad(s, bl, br, add(br, scale(d, T)), add(bl, scale(d, T)), W, H);
}

// --- pyramid ------------------------------------------------------------
//
// The front face upright at z = -RT, so the solid's centroid is the
// origin and it tumbles about it. A flap on edge (p, q) -- in the front
// face's clockwise order -- has its free corner at the edge's midpoint
// plus a face height along (cos, sin) of the hinge angle, between the
// outward in-plane direction and +z. It runs (x, q, p) to stay clockwise.
static void build_pyramid(struct usolid *s, const fx_t *stage, int W, int H) {
    struct geom_pt3 c[3] = {
        P(0, -TET_RC, -TET_RT), P(TET_HALF, TET_RI, -TET_RT), P(-TET_HALF, TET_RI, -TET_RT),
    };
    int a = vert(s, c[0], W / 2, 0), b = vert(s, c[1], W, H), d = vert(s, c[2], 0, H);
    tri(s, a, b, d);
    for (int e = 0; e < 3; e++) {
        struct geom_pt3 p = c[e], q = c[(e + 1) % 3];
        struct geom_pt3 mid = P((p.x + q.x) / 2, (p.y + q.y) / 2, -TET_RT);
        // From the face's centre to an edge's midpoint is the inradius,
        // and perpendicular to the edge.
        struct geom_pt3 out = P(fx_div(mid.x, TET_RI), fx_div(mid.y, TET_RI), 0);
        fx_t h = hinge(stage, e, TET_CLOSED);
        struct geom_pt3 dir = add(scale(out, fx_cos(h)), P(0, 0, fx_sin(h)));
        struct geom_pt3 x = add(mid, scale(dir, TET_HF));
        int i = vert(s, x, W / 2, 0), j = vert(s, q, W, H), k = vert(s, p, 0, H);
        tri(s, i, j, k);
    }
}

// --- ball ---------------------------------------------------------------
//
// A sheet 2 pi r by pi r at z = -r ROLLS into a tube -- at progress b the
// sheet is an arc of b turns, radius r / b, still touching z = -r in the
// middle -- and the tube then PINCHES into the sphere, every corner moving
// in a straight line from where the tube had it to where the globe does.
// Longitude is u and latitude v, so the picture wraps once round.
static void build_ball(struct usolid *s, const fx_t *stage, int W, int H) {
    fx_t roll = stage ? stage[0] : FX_ONE, pinch = stage ? stage[1] : FX_ONE;
    if (roll < 0) roll = 0;
    if (roll > FX_ONE) roll = FX_ONE;
    if (pinch < 0) pinch = 0;
    if (pinch > FX_ONE) pinch = FX_ONE;
    if (pinch > 0) roll = FX_ONE;
    s->smooth = 1;

    for (int j = 0; j <= BALL_LAT; j++) {
        fx_t v = j * FX_ONE / BALL_LAT - FX_HALF;      // -1/2 .. 1/2
        fx_t y = fx_mul(v, BALL_H);
        for (int i = 0; i <= BALL_LON; i++) {
            fx_t u = i * FX_ONE / BALL_LON - FX_HALF;
            struct geom_pt3 p, n;
            // BELOW 1/256 OF A TURN THE SHEET IS FLAT: r / b overflows
            // Q16.16 long before the arc is visible.
            if (roll < FX_ONE / 256) {
                p = P(fx_mul(u, BALL_W), y, -BALL_R);
                n = P(0, 0, -FX_ONE);
            } else {
                fx_t phi = fx_mul(u, roll);
                fx_t rho = fx_div(BALL_R, roll);
                fx_t sn = fx_sin(phi), cs = fx_cos(phi);
                p = P(fx_mul(rho, sn), y, -BALL_R + fx_mul(rho, FX_ONE - cs));
                n = P(sn, 0, -cs);
            }
            if (pinch > 0) {
                fx_t lat = v / 2;                       // a quarter turn each way
                fx_t cl = fx_cos(lat), sl = fx_sin(lat);
                fx_t sn = fx_sin(u), cs = fx_cos(u);
                struct geom_pt3 g = P(fx_mul(cl, sn), sl, -fx_mul(cl, cs));
                p = lerp3(p, scale(g, BALL_R), pinch);
                n = lerp3(n, g, pinch);
            }
            int k = vert(s, p, i * W / BALL_LON, j * H / BALL_LAT);
            s->v[k].n = n;
        }
    }
    for (int j = 0; j < BALL_LAT; j++)
        for (int i = 0; i < BALL_LON; i++) {
            int tl = j * (BALL_LON + 1) + i, tr = tl + 1;
            int bl = tl + BALL_LON + 1, br = bl + 1;
            tri(s, tl, tr, br);
            tri(s, tl, br, bl);
        }
}

int usolid_stages(int shape) {
    switch (shape) {
    case USOLID_CUBE:    return 5;
    case USOLID_PYRAMID: return 3;
    case USOLID_BALL:    return 2;
    }
    return 0;
}

void usolid_build(struct usolid *s, int shape, const fx_t *stage, int tw, int th) {
    // THE LAST TEXEL, NOT THE SIZE: ugfx_tex wraps u by modulo, so an edge
    // at u = tw samples column 0 down the far side of every face.
    int W = tw > 1 ? tw - 1 : 0, H = th > 1 ? th - 1 : 0;
    s->shape = shape;
    s->smooth = 0;
    s->nv = s->nt = 0;
    if (shape == USOLID_PYRAMID)   build_pyramid(s, stage, W, H);
    else if (shape == USOLID_BALL) build_ball(s, stage, W, H);
    else                           build_cube(s, stage, W, H);
}

void usolid_front(int shape, int tw, int th, struct usolid_vert q[4]) {
    int W = tw > 1 ? tw - 1 : 0, H = th > 1 ? th - 1 : 0;
    memset(q, 0, 4 * sizeof q[0]);
    if (shape == USOLID_PYRAMID) {
        q[0].p = q[1].p = P(0, -TET_RC, -TET_RT);
        q[2].p = P(TET_HALF, TET_RI, -TET_RT);
        q[3].p = P(-TET_HALF, TET_RI, -TET_RT);
        q[0].u = q[1].u = W / 2;
        q[2].u = W; q[2].v = H;
        q[3].v = H;
        return;
    }
    fx_t hx = shape == USOLID_BALL ? BALL_W / 2 : FX_ONE;
    fx_t hy = shape == USOLID_BALL ? BALL_H / 2 : FX_ONE;
    fx_t z = shape == USOLID_BALL ? -BALL_R : -FX_ONE;
    q[0].p = P(-hx, -hy, z); q[1].p = P(hx, -hy, z);
    q[2].p = P(hx, hy, z);   q[3].p = P(-hx, hy, z);
    q[1].u = W; q[2].u = W; q[2].v = H; q[3].v = H;
}

// --- slicing: where it was hit --------------------------------------------

static uint64_t isqrt64(uint64_t n) {
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

struct geom_pt3 usolid_unit(struct geom_pt3 p) {
    uint64_t q = (uint64_t)((int64_t)p.x * p.x + (int64_t)p.y * p.y + (int64_t)p.z * p.z);
    fx_t len = (fx_t)isqrt64(q);   // fx: sqrt of an fx^2 is fx
    if (len <= 0) return P(0, 0, FX_ONE);
    return P(fx_div(p.x, len), fx_div(p.y, len), fx_div(p.z, len));
}

static int64_t dot3(struct geom_pt3 a, struct geom_pt3 b) {
    return ((int64_t)a.x * b.x + (int64_t)a.y * b.y + (int64_t)a.z * b.z) >> FX_SHIFT;
}

fx_t usolid_support(const struct usolid *s, struct geom_pt3 n) {
    int64_t m = -(1ll << 40);
    for (int i = 0; i < s->nv; i++) {
        int64_t d = dot3(n, s->v[i].p);
        if (d > m) m = d;
    }
    return (fx_t)m;
}

// Which way (b - a) x (c - a) points along `out`: the mesh's rule is that
// it points IN (clockwise from outside), so a positive answer is a
// triangle to turn round.
static int64_t facing(struct geom_pt3 a, struct geom_pt3 b, struct geom_pt3 c, struct geom_pt3 out) {
    int64_t ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    int64_t vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    int64_t nx = (uy * vz - uz * vy) >> FX_SHIFT, ny = (uz * vx - ux * vz) >> FX_SHIFT;
    int64_t nz = (ux * vy - uy * vx) >> FX_SHIFT;
    return nx * out.x + ny * out.y + nz * out.z;
}

// The scratch a slice builds into: past a ring-3 stack frame, and the
// same size as the mesh it replaces.
static struct usolid_vert g_sv[USOLID_VERT_MAX];
static uint16_t g_st[USOLID_TRI_MAX][3];
static int g_snv, g_snt, g_full;   // g_full: a slot ran out, the slice is abandoned
static struct { uint16_t a, b, v; } g_edge[USOLID_VERT_MAX];
static int g_nedge;

static int sv_add(struct usolid_vert v) {
    if (g_snv >= USOLID_VERT_MAX) { g_full = 1; return 0; }
    g_sv[g_snv] = v;
    return g_snv++;
}

static void st_add(int a, int b, int c) {
    if (g_snt >= USOLID_TRI_MAX) { g_full = 1; return; }
    g_st[g_snt][0] = (uint16_t)a;
    g_st[g_snt][1] = (uint16_t)b;
    g_st[g_snt][2] = (uint16_t)c;
    g_snt++;
}

// Where the plane crosses edge i-j, made once and shared by both sides.
static int split(const struct usolid *s, const int64_t *side, const int *keep, int i, int j) {
    int a = i < j ? i : j, b = i < j ? j : i;
    for (int k = 0; k < g_nedge; k++)
        if (g_edge[k].a == a && g_edge[k].b == b) return g_edge[k].v;
    // FROM THE SAME END, whichever copy of an edge this is: a cube's faces
    // carry their own corners, so one edge is two index pairs, and
    // interpolating each from its own end left the two crossings a
    // rounding apart -- a crack the cap then bridged on one side only.
    int lo = a, hi = b;
    {
        struct geom_pt3 pa = s->v[a].p, pb = s->v[b].p;
        if (pa.x > pb.x || (pa.x == pb.x && (pa.y > pb.y || (pa.y == pb.y && pa.z > pb.z)))) { lo = b; hi = a; }
    }
    const struct usolid_vert *p = &s->v[lo], *q = &s->v[hi];
    int64_t den = side[lo] - side[hi];
    fx_t t = den ? (fx_t)(side[lo] * FX_ONE / den) : FX_HALF;   // 0 at lo, 1 at hi
    struct usolid_vert v;
    v.p = P(p->p.x + fx_mul(q->p.x - p->p.x, t), p->p.y + fx_mul(q->p.y - p->p.y, t),
            p->p.z + fx_mul(q->p.z - p->p.z, t));
    v.n = P(p->n.x + fx_mul(q->n.x - p->n.x, t), p->n.y + fx_mul(q->n.y - p->n.y, t),
            p->n.z + fx_mul(q->n.z - p->n.z, t));
    v.u = p->u + (int)(((int64_t)(q->u - p->u) * t) >> FX_SHIFT);
    v.v = p->v + (int)(((int64_t)(q->v - p->v) * t) >> FX_SHIFT);
    int idx = sv_add(v);
    if (g_nedge < USOLID_VERT_MAX) g_edge[g_nedge++] = (typeof(g_edge[0])){ (uint16_t)a, (uint16_t)b, (uint16_t)idx };
    (void)keep;
    return idx;
}

// A cheap angle that only has to SORT: 0..4 round the circle, in fx.
static fx_t pseudo_angle(int64_t x, int64_t y) {
    int64_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    if (ax + ay == 0) return 0;
    fx_t r = (fx_t)(y * FX_ONE / (ax + ay));   // -1..1
    return x >= 0 ? (y >= 0 ? r : 4 * FX_ONE + r) : 2 * FX_ONE - r;
}

void usolid_slice(struct usolid *s, struct geom_pt3 n, fx_t off, int interior_v, int tw) {
    static int64_t side[USOLID_VERT_MAX];
    static int keep[USOLID_VERT_MAX], remap[USOLID_VERT_MAX];
    int kept = 0;
    for (int i = 0; i < s->nv; i++) {
        side[i] = dot3(n, s->v[i].p) - off;
        keep[i] = side[i] <= 0;
        kept += keep[i];
    }
    if (kept == s->nv || kept == 0) return;   // nothing to take, or nothing left

    // The kept corners first, so their indices map one to one.
    g_snv = g_snt = g_nedge = g_full = 0;
    for (int i = 0; i < s->nv; i++) remap[i] = keep[i] ? sv_add(s->v[i]) : -1;
    int first_new = g_snv;

    // EVERY FACE CLIPPED, Sutherland-Hodgman on three corners: what is
    // kept, plus a point wherever an edge crosses. Winding is preserved.
    for (int t = 0; t < s->nt; t++) {
        int out[4], m = 0;
        for (int k = 0; k < 3; k++) {
            int i = s->t[t][k], j = s->t[t][(k + 1) % 3];
            if (keep[i]) out[m++] = remap[i];
            if (keep[i] != keep[j] && m < 4) out[m++] = split(s, side, keep, i, j);
        }
        for (int k = 1; k + 1 < m; k++) st_add(out[0], out[k], out[k + 1]);
    }

    // CROSSINGS A ROUNDING APART ARE ONE POINT: welded, so the faces and
    // the cap meet without a crack. Edges that are one edge in space but
    // different corners in the mesh (a seam, a face's own copy, an earlier
    // cap) cross the plane a few units apart.
    for (int i = first_new; i < g_snv; i++)
        for (int j = first_new; j < i; j++) {
            struct geom_pt3 a = g_sv[j].p, b = g_sv[i].p;
            if ((a.x - b.x) * (int64_t)(a.x - b.x) + (a.y - b.y) * (int64_t)(a.y - b.y) +
                (a.z - b.z) * (int64_t)(a.z - b.z) < (int64_t)(FX_ONE / 256) * (FX_ONE / 256)) {
                g_sv[i].p = a;
                break;
            }
        }

    // THE CAP: the crossings, round the plane in order, fanned from their
    // centre. Points at one place (a texture seam) are one point.
    static int ring[USOLID_VERT_MAX];
    static fx_t key[USOLID_VERT_MAX];
    int nr = 0;
    for (int i = first_new; i < g_snv; i++) {
        int dup = 0;
        for (int k = 0; k < nr && !dup; k++) {
            struct geom_pt3 a = g_sv[ring[k]].p, b = g_sv[i].p;
            dup = (a.x - b.x) * (int64_t)(a.x - b.x) + (a.y - b.y) * (int64_t)(a.y - b.y) +
                  (a.z - b.z) * (int64_t)(a.z - b.z) < (int64_t)(FX_ONE / 256) * (FX_ONE / 256);
        }
        if (!dup) ring[nr++] = i;
    }
    if (nr >= 3 && g_snv + nr + 1 <= USOLID_VERT_MAX) {
        struct geom_pt3 c = P(0, 0, 0);
        for (int k = 0; k < nr; k++) { c.x += g_sv[ring[k]].p.x / nr; c.y += g_sv[ring[k]].p.y / nr; c.z += g_sv[ring[k]].p.z / nr; }
        // Two directions in the plane, to measure each point's angle by.
        struct geom_pt3 ax = (n.x > FX_ONE / 2 || n.x < -FX_ONE / 2) ? P(0, FX_ONE, 0) : P(FX_ONE, 0, 0);
        struct geom_pt3 e1 = usolid_unit(P(fx_mul(n.y, ax.z) - fx_mul(n.z, ax.y), fx_mul(n.z, ax.x) - fx_mul(n.x, ax.z),
                                           fx_mul(n.x, ax.y) - fx_mul(n.y, ax.x)));
        struct geom_pt3 e2 = P(fx_mul(n.y, e1.z) - fx_mul(n.z, e1.y), fx_mul(n.z, e1.x) - fx_mul(n.x, e1.z),
                               fx_mul(n.x, e1.y) - fx_mul(n.y, e1.x));
        for (int k = 0; k < nr; k++) {
            struct geom_pt3 d = g_sv[ring[k]].p;
            d = P(d.x - c.x, d.y - c.y, d.z - c.z);
            key[k] = pseudo_angle(dot3(d, e1), dot3(d, e2));
        }
        for (int i = 1; i < nr; i++)   // insertion sort: a cap has tens of points
            for (int j = i; j > 0 && key[j] < key[j - 1]; j--) {
                fx_t tk = key[j]; key[j] = key[j - 1]; key[j - 1] = tk;
                int tr = ring[j]; ring[j] = ring[j - 1]; ring[j - 1] = tr;
            }
        // The cap's own corners, textured from the interior rows and lit
        // as the flat face it is.
        int base = g_snv, ci;
        for (int k = 0; k < nr; k++) {
            struct usolid_vert v = { g_sv[ring[k]].p, n, tw > 8 ? (k * 37) % tw : 0, interior_v + (k % 3) * 3 };
            sv_add(v);
        }
        ci = sv_add((struct usolid_vert){ c, n, tw > 8 ? tw / 2 : 0, interior_v + 3 });
        for (int k = 0; k < nr; k++) {
            int a = base + k, b = base + (k + 1) % nr;
            if (facing(g_sv[ci].p, g_sv[a].p, g_sv[b].p, n) > 0) st_add(ci, b, a);
            else st_add(ci, a, b);
        }
    }
    // OUT OF ROOM: the solid is left as it was rather than with a hole --
    // a cut that does not happen is invisible, a hole is not.
    if (g_full || nr < 3) return;
    memcpy(s->v, g_sv, (size_t)g_snv * sizeof g_sv[0]);
    memcpy(s->t, g_st, (size_t)g_snt * sizeof g_st[0]);
    s->nv = g_snv;
    s->nt = g_snt;
}

// --- drawing ------------------------------------------------------------

// Ambient so a face turned from the light is dim rather than black: a
// desktop on a face nobody can read is no longer a desktop.
static int lit_level(struct geom_pt3 n) { return 110 + geom_shade(n, LIGHT) * 145 / 255; }

static struct usolid_proj project_rotated(const struct usolid_view *v, struct geom_pt3 r) {
    int64_t X = ((int64_t)r.x * v->unit) >> FX_SHIFT;
    int64_t Y = ((int64_t)r.y * v->unit) >> FX_SHIFT;
    int64_t Z = ((int64_t)r.z * v->unit) >> FX_SHIFT;
    int64_t depth = v->dist + Z;
    if (depth < 1) depth = 1;
    struct usolid_proj p = { (int)(X * v->dist / depth), (int)(Y * v->dist / depth), (int)depth, 255 };
    return p;
}

struct usolid_proj usolid_project_pt(const struct usolid_view *v, struct geom_pt3 p) {
    return project_rotated(v, geom_rotate3(p, v->yaw, v->pitch, v->roll));
}

void usolid_project(struct usolid *s, const struct usolid_view *v, struct usolid_box *box) {
    box->x0 = box->y0 = 1 << 30;
    box->x1 = box->y1 = -(1 << 30);
    for (int i = 0; i < s->nv; i++) {
        struct geom_pt3 r = geom_rotate3(s->v[i].p, v->yaw, v->pitch, v->roll);
        s->rp[i] = r;
        struct usolid_proj *p = &s->pv[i];
        *p = project_rotated(v, r);
        if (v->lit && s->smooth)
            p->shade = lit_level(geom_rotate3(s->v[i].n, v->yaw, v->pitch, v->roll));
        if (p->x < box->x0) box->x0 = p->x;
        if (p->y < box->y0) box->y0 = p->y;
        if (p->x > box->x1) box->x1 = p->x;
        if (p->y > box->y1) box->y1 = p->y;
    }
    for (int t = 0; t < s->nt; t++) {
        s->fshade[t] = 255;
        if (!v->lit || s->smooth) continue;
        const uint16_t *f = s->t[t];
        struct geom_pt3 n = geom_face_normal3(s->rp[f[0]], s->rp[f[1]], s->rp[f[2]]);
        // Clockwise from outside makes (b - a) x (c - a) point IN.
        n = P(-n.x, -n.y, -n.z);
        s->fshade[t] = (uint8_t)lit_level(n);
    }
}

void usolid_draw(struct ugfx_surface *surf, struct usolid *s, const struct ugfx_texture *tex,
                 int cx, int cy, fx_t sx, fx_t sy) {
    int n = 0;
    for (int t = 0; t < s->nt; t++) {
        const uint16_t *f = s->t[t];
        const struct usolid_proj *a = &s->pv[f[0]], *b = &s->pv[f[1]], *c = &s->pv[f[2]];
        int ax = fx_mul(a->x, sx), ay = fx_mul(a->y, sy);
        int bx = fx_mul(b->x, sx), by = fx_mul(b->y, sy);
        int qx = fx_mul(c->x, sx), qy = fx_mul(c->y, sy);
        int64_t cross = (int64_t)(bx - ax) * (qy - ay) - (int64_t)(by - ay) * (qx - ax);
        if (cross <= 0) continue;   // wound away from the eye, or edge-on
        s->order[n++] = (uint16_t)t;
    }
    for (int i = 0; i < n; i++) {
        const uint16_t *f = s->t[s->order[i]];
        s->key[s->order[i]] = s->pv[f[0]].z + s->pv[f[1]].z + s->pv[f[2]].z;
    }
    // FAR TO NEAR. A Shell sort: the ball has a few hundred faces facing
    // the eye, which an insertion sort makes quadratic.
    for (int gap = n / 2; gap > 0; gap /= 2)
        for (int i = gap; i < n; i++) {
            uint16_t o = s->order[i];
            int32_t k = s->key[o];
            int j = i;
            while (j >= gap && s->key[s->order[j - gap]] < k) {
                s->order[j] = s->order[j - gap];
                j -= gap;
            }
            s->order[j] = o;
        }

    for (int i = 0; i < n; i++) {
        int t = s->order[i];
        struct ugfx_texvert tv[3];
        for (int k = 0; k < 3; k++) {
            int vi = s->t[t][k];
            const struct usolid_proj *p = &s->pv[vi];
            tv[k].x = cx + fx_mul(p->x, sx);
            tv[k].y = cy + fx_mul(p->y, sy);
            tv[k].z = p->z;
            tv[k].u = s->v[vi].u;
            tv[k].v = s->v[vi].v;
            tv[k].shade = s->smooth ? p->shade : s->fshade[t];
        }
        ugfx_tri3d(surf, NULL, tex, 0, &tv[0], &tv[1], &tv[2]);
    }
}

int usolid_bounce(struct usolid_motion *m, const struct usolid_box *box,
                  int w, int h, int dt_ms) {
    m->x256 += (int)((int64_t)m->vx * dt_ms * 256 / 1000);
    m->y256 += (int)((int64_t)m->vy * dt_ms * 256 / 1000);
    int hit = 0;
    int x = m->x256 >> 8, y = m->y256 >> 8;
    // A box wider than the screen can touch neither side cleanly; centre
    // it on that axis rather than flipping every frame.
    if (box->x1 - box->x0 >= w) {
        m->x256 = (w / 2) << 8;
    } else if (x + box->x0 < 0) {
        m->x256 = (-box->x0) << 8;
        if (m->vx < 0) { m->vx = -m->vx; hit |= USOLID_HIT_LEFT; }
    } else if (x + box->x1 >= w) {
        m->x256 = (w - 1 - box->x1) << 8;
        if (m->vx > 0) { m->vx = -m->vx; hit |= USOLID_HIT_RIGHT; }
    }
    if (box->y1 - box->y0 >= h) {
        m->y256 = (h / 2) << 8;
    } else if (y + box->y0 < 0) {
        m->y256 = (-box->y0) << 8;
        if (m->vy < 0) { m->vy = -m->vy; hit |= USOLID_HIT_TOP; }
    } else if (y + box->y1 >= h) {
        m->y256 = (h - 1 - box->y1) << 8;
        if (m->vy > 0) { m->vy = -m->vy; hit |= USOLID_HIT_BOTTOM; }
    }
    return hit;
}
