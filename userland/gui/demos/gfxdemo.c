// Shapes -- a ring-3 client demonstrating the geometry primitives.
//
// Three scenes on one canvas: a rotating triangle and ellipse, a cube
// that is a wireframe or a lit solid, and the Utah teapot. Every vertex
// goes through the shared geometry module (kernel/lib/geom.c) -- the
// same Bresenham, the same ellipse rasteriser, the same fixed-point trig
// the KERNEL uses, compiled a second time for ring 3 -- and the solids
// are filled by ui/ugfx_tex.h's triangle, the teapot through a depth
// buffer because it is not convex.
//
// The window is RESIZABLE: the canvas takes what the toolbar leaves and
// every scene scales to the canvas's smaller side. The toolbar is a
// uui_toolbar with overflow on, so as the window narrows the view
// toggles fold into its "View" menu rather than running off the edge.
//
// Every state change is logged as one parseable line (`gfxdemo: aa on`)
// so tools/gfxdemo_test.py can assert on behaviour rather than pixels
// alone -- the grammar apps/uidemo.c established.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ulog.h"
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uui_toolbar.h"
#include "ui/ugfx_tex.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "geom.h"
#include "fixed.h"
#include "keyboard.h"
#include "shapes/teapot.h"

#define MARGIN 10

#define SCENE_2D     0
#define SCENE_CUBE   1
#define SCENE_TEAPOT 2
static const char *const SCENE_NAME[3] = { "2d", "cube", "teapot" };

// The toolbar's commands, which are also the keys' (act() below).
enum {
    CMD_2D = 1, CMD_CUBE, CMD_TEAPOT,
    CMD_SLOWER, CMD_FASTER, CMD_RESET,
    CMD_AA, CMD_SHADE, CMD_TEX,
};

#define ID_TOOLBAR 1

// The speed readout's text, rewritten by set_speed(); the toolbar item
// below points at it and re-measures every draw.
static char g_speed_text[16];

// Scenes and speed stay in the strip; from Reset on is what folds into
// the View menu first. KEEP_SHOWN is how many must always fit -- the
// window's minimum width is measured from it (on_size).
static const struct uui_toolbar_item TOOLBAR[] = {
    { 0, "2D shapes (1)", CMD_2D,     "2D",     0, "1", 0 },
    { 0, "Cube (2)",      CMD_CUBE,   "Cube",   0, "2", 0 },
    { 0, "Teapot (3)",    CMD_TEAPOT, "Teapot", 0, "3", 0 },
    UUI_TOOLBAR_SEP,
    { 0, "Slower (-)",    CMD_SLOWER, "-",      0, "-", 0 },
    { 0, 0,               0,          g_speed_text, UUI_TB_TEXT, 0, 0 },
    { 0, "Faster (+)",    CMD_FASTER, "+",      0, "+", 0 },
    UUI_TOOLBAR_SEP,
    { 0, "Reset speed and angle (R)", CMD_RESET, "Reset", 0, "R", 0 },
    { 0, "Anti-aliased lines (A)", CMD_AA,  "Smooth edges", UUI_TB_END, "A", 0 },
    { 0, "Filled and lit (F)",     CMD_SHADE, "Shaded",     0, "F", 0 },
    { 0, "Textured (T)",           CMD_TEX,   "Textured",   0, "T", 0 },
};
#define TOOLBAR_N ((int)(sizeof TOOLBAR / sizeof TOOLBAR[0]))
#define KEEP_SHOWN 7

static struct uui_toolbar g_tb;
static struct uui_item g_items[] = {
    { .ops = &uui_toolbar_ops, .widget = &g_tb, .id = ID_TOOLBAR, .name = "toolbar" },
};

static struct uapp_desc g_desc;
static int g_w, g_h;
static struct uui_canvas g_canvas;

// State. Anti-aliasing starts on; shading and texturing off, because
// the wireframe is the demo's proof that the projection is real and a
// solid hides the far edges that show it.
static int g_aa = 1, g_shade, g_tex;
static int g_scene = SCENE_2D;
static fx_t g_angle;            // in turns; wraps naturally
static int g_speed = 3;         // 1/1024 turns per 10 ms

// THE TEXTURE, generated rather than loaded: a checkerboard is what
// makes a mapping error obvious at a glance, which is the whole reason
// to look at a textured solid in a demo.
#define TEX_SIZE 64
static uint32_t g_tex_px[TEX_SIZE * TEX_SIZE];
static struct ugfx_texture g_texture = { g_tex_px, TEX_SIZE, TEX_SIZE };

// The depth buffer the teapot draws through, sized to the canvas and
// regrown when a resize makes it bigger (zb_fit()).
static struct ugfx_zbuffer g_zb;
static int g_zb_cap;

// Where the light comes from: above, to the left, and in front (y is
// down, z is away). Fixed in the world, so a face brightens and dims as
// it turns through the beam -- which is what makes a rotation legible
// as a solid.
static const struct geom_pt3 LIGHT = { -FX_ONE, -FX_ONE * 3 / 2, -FX_ONE };

// Lambert over an ambient floor, 70..255: a face turned from the light
// is dim rather than black, and black reads as a hole.
static int lit_level(struct geom_pt3 n) { return 70 + geom_shade(n, LIGHT) * 185 / 255; }

// The scenes were drawn for a canvas about this tall and scale from it;
// the 2D scene's outer ring is 300 across, so this leaves it a margin.
#define REF_SIDE 330

static int canvas_side(void) { return g_canvas.w < g_canvas.h ? g_canvas.w : g_canvas.h; }

static fx_t zoom(void) {
    fx_t z = fx_div(fx_from_int(canvas_side()), fx_from_int(REF_SIDE));
    return z < FX_ONE / 4 ? FX_ONE / 4 : z;
}

static int scaled(int px) { return fx_round(fx_mul(fx_from_int(px), zoom())); }

// --- the log ------------------------------------------------------------

static void log_speed(void) { ulogf("gfxdemo: speed %d\n", g_speed); }
static void log_scene(void) { ulogf("gfxdemo: scene %s\n", SCENE_NAME[g_scene]); }

static void set_speed(int v) {
    g_speed = v < 0 ? 0 : v > 40 ? 40 : v;
    snprintf(g_speed_text, sizeof g_speed_text, "speed %d", g_speed);
}

// --- layout ---------------------------------------------------------------

static void layout(void) {
    int tb_h = uui_toolbar_height(&g_tb);
    uui_toolbar_ops.set_geometry(&g_tb, 0, 0, g_w, tb_h);
    uui_toolbar_set_bounds(&g_tb, 0, 0, g_w, g_h);
    int ch = g_h - tb_h - 2 * MARGIN;
    uui_canvas_init(&g_canvas, MARGIN, tb_h + MARGIN, g_w - 2 * MARGIN, ch < 1 ? 1 : ch,
                    ugfx_rgb(16, 18, 24), ugfx_rgb(70, 78, 92));
}

// The canvas's inside, in surface coordinates -- the border stays out
// of reach of the solids, which draw straight onto the surface.
static void canvas_inner(int *x, int *y, int *w, int *h) {
    *x = g_canvas.x + 1; *y = g_canvas.y + 1;
    *w = g_canvas.w - 2; *h = g_canvas.h - 2;
}

// Enough depth buffer for the canvas as it is now. A failed malloc
// leaves none, and the teapot then draws unsorted rather than not at all.
static void zb_fit(void) {
    int x, y, w, h;
    canvas_inner(&x, &y, &w, &h);
    if (w < 1 || h < 1) {
        free(g_zb.depth);
        g_zb.depth = 0;
        g_zb_cap = 0;
        return;
    }
    if (w * h > g_zb_cap) {
        free(g_zb.depth);
        g_zb.depth = malloc((size_t)w * h * sizeof g_zb.depth[0]);
        g_zb_cap = g_zb.depth ? w * h : 0;
    }
    g_zb.x = x; g_zb.y = y; g_zb.w = w; g_zb.h = h;
}

// --- the cube ---------------------------------------------------------
//
// Eight corners and twelve edges, and that is the whole model -- every
// rotation and the perspective divide come from geom_transform3(). Half
// size 80 against a projection distance of 320, both scaled with the
// canvas: near enough that the front face is visibly larger than the
// back one. Much further and a face-on cube collapses into one square,
// the degenerate case tools/gfxdemo_test.py asserts against.
#define CUBE_HALF 80
#define CUBE_DIST fx_from_int(320)

static const struct geom_pt3 CUBE[8] = {
    { -CUBE_HALF * FX_ONE, -CUBE_HALF * FX_ONE, -CUBE_HALF * FX_ONE },
    {  CUBE_HALF * FX_ONE, -CUBE_HALF * FX_ONE, -CUBE_HALF * FX_ONE },
    {  CUBE_HALF * FX_ONE,  CUBE_HALF * FX_ONE, -CUBE_HALF * FX_ONE },
    { -CUBE_HALF * FX_ONE,  CUBE_HALF * FX_ONE, -CUBE_HALF * FX_ONE },
    { -CUBE_HALF * FX_ONE, -CUBE_HALF * FX_ONE,  CUBE_HALF * FX_ONE },
    {  CUBE_HALF * FX_ONE, -CUBE_HALF * FX_ONE,  CUBE_HALF * FX_ONE },
    {  CUBE_HALF * FX_ONE,  CUBE_HALF * FX_ONE,  CUBE_HALF * FX_ONE },
    { -CUBE_HALF * FX_ONE,  CUBE_HALF * FX_ONE,  CUBE_HALF * FX_ONE },
};

// Front face, back face, then the four struts joining them -- an edge
// list, because a cube's edges do not form one path.
static const uint8_t CUBE_EDGES[12][2] = {
    {0,1},{1,2},{2,3},{3,0},
    {4,5},{5,6},{6,7},{7,4},
    {0,4},{1,5},{2,6},{3,7},
};

// The six faces, each wound so geom_face_normal3() points OUT of the
// cube: a convex solid needs no hidden-surface work beyond dropping the
// faces that face away.
static const uint8_t CUBE_FACES[6][4] = {
    {0,3,2,1}, {4,5,6,7}, {0,1,5,4}, {3,7,6,2}, {0,4,7,3}, {1,2,6,5},
};

// Near is bright, far is dim, from a depth `z` within +-`half`.
static uint32_t depth_color(fx_t z, fx_t half) {
    fx_t k = fx_div(z + half, 2 * half);
    int t = fx_round(fx_mul(k, fx_from_int(255)));
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    int shade = 235 - t * 150 / 255;   // 235 near .. 85 far
    return ugfx_rgb((uint8_t)shade, (uint8_t)(shade * 4 / 5), (uint8_t)(90 + shade / 3));
}

static void draw_cube(struct ugfx_surface *s, int cx, int cy, enum geom_aa aa) {
    int xs[8], ys[8];
    fx_t z[8];
    fx_t k = zoom(), half = fx_mul(fx_from_int(CUBE_HALF), k), dist = fx_mul(CUBE_DIST, k);

    // Two axes, at rates whose ratio is not a simple fraction: a cube
    // yawing alone shows the same silhouette four times a turn.
    fx_t yaw = g_angle, pitch = fx_mul(g_angle, 24248 /* ~0.37 */);
    geom_transform3(CUBE, 8, yaw, pitch, 0, k, dist, cx, cy, xs, ys, z);

    if (g_shade || g_tex) {
        // The rotated corners again, unscaled: a face's normal needs all
        // three coordinates and geom_transform3() hands back only depth.
        struct geom_pt3 r[8];
        for (int i = 0; i < 8; i++) r[i] = geom_rotate3(CUBE[i], yaw, pitch, 0);

        for (int f = 0; f < 6; f++) {
            const uint8_t *q = CUBE_FACES[f];
            struct geom_pt3 n = geom_face_normal3(r[q[0]], r[q[1]], r[q[2]]);
            // Front-facing when the normal has a component toward the eye
            // at (0, 0, -dist) -- which, under perspective, is not quite
            // "n.z < 0" for a face seen nearly edge-on.
            int64_t vx = -r[q[0]].x, vy = -r[q[0]].y, vz = -(int64_t)CUBE_DIST - r[q[0]].z;
            if ((int64_t)n.x * vx + (int64_t)n.y * vy + (int64_t)n.z * vz <= 0) continue;
            int lit = lit_level(n);

            if (g_tex) {
                // The texture's corners in the face's winding order, so the
                // image is upright on every face rather than mirrored on half.
                static const int UV[4][2] = {
                    { 0, 0 }, { TEX_SIZE - 1, 0 },
                    { TEX_SIZE - 1, TEX_SIZE - 1 }, { 0, TEX_SIZE - 1 },
                };
                struct ugfx_texvert tv[4];
                for (int i = 0; i < 4; i++) {
                    tv[i].x = g_canvas.x + xs[q[i]];
                    tv[i].y = g_canvas.y + ys[q[i]];
                    // VIEW depth: geom's z is from the model's centre, so
                    // the eye distance goes back on. Both fx_t, converted once.
                    tv[i].z = fx_to_int(dist + z[q[i]]);
                    if (tv[i].z < 1) tv[i].z = 1;
                    tv[i].u = UV[i][0];
                    tv[i].v = UV[i][1];
                }
                ugfx_textured_quad(s, &g_texture, tv, lit);
                continue;
            }
            uint32_t color = ugfx_rgb((uint8_t)(90 * lit / 255), (uint8_t)(200 * lit / 255),
                                      (uint8_t)(250 * lit / 255));
            int fx4[4], fy4[4];
            for (int i = 0; i < 4; i++) { fx4[i] = xs[q[i]]; fy4[i] = ys[q[i]]; }
            // No outline: two lit faces meet at a change of shade, and a
            // line there reads as the wireframe showing through.
            uui_canvas_fill_polygon(s, &g_canvas, fx4, fy4, 4, color);
        }
        return;   // no wireframe through a solid, and no corner dots
    }

    // Edges shaded by DEPTH -- the visible proof the projection is real.
    for (int e = 0; e < 12; e++) {
        int a = CUBE_EDGES[e][0], b = CUBE_EDGES[e][1];
        uui_canvas_line(s, &g_canvas, xs[a], ys[a], xs[b], ys[b],
                        depth_color((z[a] + z[b]) / 2, half), aa);
    }
    // A dot at each corner, sized by depth, which makes a wrong
    // projection obvious at a glance.
    for (int i = 0; i < 8; i++) {
        fx_t nearness = fx_div(half - z[i], 2 * half);
        int r = 2 + fx_round(fx_mul(nearness, fx_from_int(4)));
        if (r < 2) r = 2;
        uui_canvas_fill_ellipse(s, &g_canvas, xs[i], ys[i], r, r, ugfx_rgb(250, 230, 180));
    }
}

// --- the teapot ---------------------------------------------------------

static struct teapot g_pot;
static int g_vx[TEAPOT_VERTS], g_vy[TEAPOT_VERTS];
static fx_t g_vz[TEAPOT_VERTS];
static uint8_t g_vk[TEAPOT_VERTS];   // Gouraud: the Lambert level per vertex
// Wireframe edges already drawn this frame: (i,j)-(i,j+1) and (i,j)-(i+1,j).
static uint8_t g_edge_u[TEAPOT_PATCHES][TEAPOT_GRID][TEAPOT_DIV];
static uint8_t g_edge_v[TEAPOT_PATCHES][TEAPOT_DIV][TEAPOT_GRID];

// Tipped toward the viewer so the lid shows, and turning on its own
// vertical axis -- a turntable, where the cube tumbles.
#define TEAPOT_TILT (FX_ONE * 7 / 100)

static int front(int a, int b, int c) {
    return (int64_t)(g_vx[b] - g_vx[a]) * (g_vy[c] - g_vy[a]) -
           (int64_t)(g_vx[c] - g_vx[a]) * (g_vy[b] - g_vy[a]) > 0;
}

static void teapot_line(struct ugfx_surface *s, int a, int b, fx_t half, enum geom_aa aa) {
    uui_canvas_line(s, &g_canvas, g_vx[a] - g_canvas.x, g_vy[a] - g_canvas.y,
                    g_vx[b] - g_canvas.x, g_vy[b] - g_canvas.y,
                    depth_color((g_vz[a] + g_vz[b]) / 2, half), aa);
}

static void draw_teapot(struct ugfx_surface *s, int cx, int cy, enum geom_aa aa) {
    // Its bounding sphere at 42% of the canvas's smaller side, the eye
    // four radii away: the same gentle perspective as the cube.
    int r_px = canvas_side() * 42 / 100;
    if (r_px < 8) return;
    fx_t scale = fx_div(fx_from_int(r_px), g_pot.radius);
    fx_t dist = fx_from_int(4 * r_px), half = fx_from_int(r_px);
    geom_transform3(g_pot.pos, TEAPOT_VERTS, g_angle, TEAPOT_TILT, 0, scale, dist,
                    g_canvas.x + cx, g_canvas.y + cy, g_vx, g_vy, g_vz);

    if (!g_shade && !g_tex) {
        // THE GRID, NOT THE TRIANGLES -- the patch's own parameter lines,
        // each drawn once, for every cell facing the eye. No depth test
        // for lines, so what is behind a nearer front face still shows:
        // this is a wireframe, not hidden-line removal.
        memset(g_edge_u, 0, sizeof g_edge_u);
        memset(g_edge_v, 0, sizeof g_edge_v);
        for (int p = 0; p < TEAPOT_PATCHES; p++)
            for (int i = 0; i < TEAPOT_DIV; i++)
                for (int j = 0; j < TEAPOT_DIV; j++) {
                    int t[6];
                    teapot_cell_tris(p, i, j, t);
                    if (!front(t[0], t[1], t[2]) && !front(t[3], t[4], t[5])) continue;
                    g_edge_u[p][i][j] = g_edge_u[p][i + 1][j] = 1;
                    g_edge_v[p][i][j] = g_edge_v[p][i][j + 1] = 1;
                }
        for (int p = 0; p < TEAPOT_PATCHES; p++)
            for (int i = 0; i <= TEAPOT_DIV; i++)
                for (int j = 0; j <= TEAPOT_DIV; j++) {
                    if (j < TEAPOT_DIV && g_edge_u[p][i][j])
                        teapot_line(s, teapot_vert(p, i, j), teapot_vert(p, i, j + 1), half, aa);
                    if (i < TEAPOT_DIV && g_edge_v[p][i][j])
                        teapot_line(s, teapot_vert(p, i, j), teapot_vert(p, i + 1, j), half, aa);
                }
        return;
    }

    for (int i = 0; i < TEAPOT_VERTS; i++)
        g_vk[i] = (uint8_t)lit_level(geom_rotate3(g_pot.nrm[i], g_angle, TEAPOT_TILT, 0));

    zb_fit();
    ugfx_zbuffer_clear(&g_zb);
    struct ugfx_zbuffer *zb = g_zb.depth ? &g_zb : 0;
    uint32_t color = ugfx_rgb(90, 200, 250);
    for (int p = 0; p < TEAPOT_PATCHES; p++)
        for (int i = 0; i < TEAPOT_DIV; i++)
            for (int j = 0; j < TEAPOT_DIV; j++) {
                int t[6];
                teapot_cell_tris(p, i, j, t);
                for (int k = 0; k < 6; k += 3) {
                    // Back faces culled first: half the triangles, and
                    // the depth buffer sorts out the rest (the handle
                    // behind the body, the lid inside the rim).
                    if (!front(t[k], t[k + 1], t[k + 2])) continue;
                    struct ugfx_texvert v[3];
                    for (int m = 0; m < 3; m++) {
                        int at = t[k + m], gi = (at / TEAPOT_GRID) % TEAPOT_GRID, gj = at % TEAPOT_GRID;
                        v[m].x = g_vx[at];
                        v[m].y = g_vy[at];
                        v[m].z = fx_to_int(dist + g_vz[at]);
                        if (v[m].z < 1) v[m].z = 1;
                        // One checker tile per patch, so the pattern
                        // follows the patch structure across the surface.
                        v[m].u = gj * TEX_SIZE / TEAPOT_DIV;
                        v[m].v = gi * TEX_SIZE / TEAPOT_DIV;
                        v[m].shade = g_vk[at];
                    }
                    ugfx_tri3d(s, zb, g_tex ? &g_texture : 0, color, &v[0], &v[1], &v[2]);
                }
            }
}

// --- the 2D scene -------------------------------------------------------

// The triangle, as points about its own centre, in pixels at REF_SIDE.
static const struct geom_pt TRI[3] = {
    { 0,               -110 * FX_ONE },
    {  95 * FX_ONE,     55 * FX_ONE },
    { -95 * FX_ONE,     55 * FX_ONE },
};

static void draw_shapes_2d(struct ugfx_surface *s, int cx, int cy, enum geom_aa aa) {
    // Static rings, to judge the curve rasteriser against while
    // everything else moves.
    uui_canvas_circle(s, &g_canvas, cx, cy, scaled(150), ugfx_rgb(38, 44, 56), aa);
    uui_canvas_circle(s, &g_canvas, cx, cy, scaled(100), ugfx_rgb(38, 44, 56), aa);

    // The rotating ellipse, its radii sweeping with the angle so the
    // rasteriser is exercised across sizes. geom_ellipse() is
    // axis-aligned -- the honest primitive -- so a rotated one is a
    // polyline of transformed points; rotation belongs to the caller.
    {
        fx_t pulse = fx_sin(g_angle * 2);
        int rx = scaled(130 + fx_round(fx_mul(pulse, fx_from_int(25))));
        int ry = scaled(60 + fx_round(fx_mul(pulse, fx_from_int(-20))));
        struct geom_pt pts[48];
        int xs[48], ys[48];
        for (int i = 0; i < 48; i++) {
            fx_t t = (fx_t)(((int64_t)i << FX_SHIFT) / 48);
            pts[i].x = fx_mul(fx_cos(t), fx_from_int(rx));
            pts[i].y = fx_mul(fx_sin(t), fx_from_int(ry));
        }
        geom_transform(pts, 48, g_angle, FX_ONE, cx, cy, xs, ys);
        uui_canvas_polyline(s, &g_canvas, xs, ys, 48, 1, ugfx_rgb(90, 200, 250), aa);
    }

    // The triangle, spinning the other way, with a dot at each vertex so
    // the filled-ellipse path is on screen too.
    {
        int xs[3], ys[3];
        geom_transform(TRI, 3, -g_angle, zoom(), cx, cy, xs, ys);
        uui_canvas_polyline(s, &g_canvas, xs, ys, 3, 1, ugfx_rgb(250, 190, 90), aa);
        for (int i = 0; i < 3; i++)
            uui_canvas_fill_ellipse(s, &g_canvas, xs[i], ys[i], 4, 4, ugfx_rgb(250, 230, 180));
    }
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    layout();
    uui_canvas_begin(s, &g_canvas);

    int cx = uui_canvas_cx(&g_canvas), cy = uui_canvas_cy(&g_canvas);
    enum geom_aa aa = g_aa ? GEOM_AA : GEOM_ALIASED;
    // The solids write the surface directly; this keeps them inside.
    int x, y, w, h;
    canvas_inner(&x, &y, &w, &h);
    ugfx_set_clip_rect(s, x, y, w, h);
    if (g_scene == SCENE_TEAPOT)    draw_teapot(s, cx, cy, aa);
    else if (g_scene == SCENE_CUBE) draw_cube(s, cx, cy, aa);
    else                            draw_shapes_2d(s, cx, cy, aa);
    ugfx_clear_clip_rect(s);

    uapp_logf_layout("gfxdemo: layout canvas %d %d %d %d\n",
                     g_canvas.x, g_canvas.y, g_canvas.w, g_canvas.h);
    uapp_log_layout(a, "gfxdemo");
}

// --- state changes, from the toolbar, its menu and the keys ------------

// One state store, asked by both presenters: the latched button and the
// ticked menu row cannot disagree (ui/uui_toolbar.h).
static unsigned item_flags(int code) {
    int solid_only = g_scene == SCENE_2D ? UUI_MI_DISABLED : 0;
    switch (code) {
    case CMD_2D:     return g_scene == SCENE_2D ? UUI_MI_CHECKED : 0;
    case CMD_CUBE:   return g_scene == SCENE_CUBE ? UUI_MI_CHECKED : 0;
    case CMD_TEAPOT: return g_scene == SCENE_TEAPOT ? UUI_MI_CHECKED : 0;
    case CMD_SLOWER: return g_speed == 0 ? UUI_MI_DISABLED : 0;
    case CMD_FASTER: return g_speed == 40 ? UUI_MI_DISABLED : 0;
    case CMD_AA:     return g_aa ? UUI_MI_CHECKED : 0;
    case CMD_SHADE:  return (g_shade ? UUI_MI_CHECKED : 0) | solid_only;
    case CMD_TEX:    return (g_tex ? UUI_MI_CHECKED : 0) | solid_only;
    default:         return 0;
    }
}

static void set_scene(int scene) {
    g_scene = scene;
    log_scene();
}

static void act(int code) {
    switch (code) {
    case CMD_2D:     set_scene(SCENE_2D); break;
    case CMD_CUBE:   set_scene(SCENE_CUBE); break;
    case CMD_TEAPOT: set_scene(SCENE_TEAPOT); break;
    case CMD_SLOWER: if (g_speed > 0) { set_speed(g_speed - 1); log_speed(); } break;
    case CMD_FASTER: if (g_speed < 40) { set_speed(g_speed + 1); log_speed(); } break;
    case CMD_RESET:  set_speed(3); g_angle = 0; log_speed(); break;
    case CMD_AA:
        g_aa = !g_aa;
        ulog(g_aa ? "gfxdemo: aa on\n" : "gfxdemo: aa off\n");
        break;
    // Shading and texture are properties of a solid: refused, and said
    // so, while the 2D scene is up.
    case CMD_SHADE:
        if (g_scene == SCENE_2D) { ulog("gfxdemo: shaded ignored -- 2d scene\n"); break; }
        g_shade = !g_shade;
        ulog(g_shade ? "gfxdemo: shaded on\n" : "gfxdemo: shaded off\n");
        break;
    case CMD_TEX:
        if (g_scene == SCENE_2D) { ulog("gfxdemo: textured ignored -- 2d scene\n"); break; }
        g_tex = !g_tex;
        ulog(g_tex ? "gfxdemo: textured on\n" : "gfxdemo: textured off\n");
        break;
    }
}

static void on_widget(struct uapp *a, int id, int reason) {
    (void)a; (void)reason;
    if (id != ID_TOOLBAR) return;
    int code = uui_toolbar_take_code(&g_tb);
    if (code > 0) act(code);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    switch (key) {
    case 'q': uapp_quit(a, 0); return;   // Esc does not close -- Alt+F4 does
    case '1': act(CMD_2D); return;
    case '2': act(CMD_CUBE); return;
    case '3': act(CMD_TEAPOT); return;
    case 's': case 'S': set_scene((g_scene + 1) % 3); return;
    case 'a': case 'A': act(CMD_AA); return;
    case 'f': case 'F': act(CMD_SHADE); return;
    case 't': case 'T': act(CMD_TEX); return;
    case 'r': case 'R': act(CMD_RESET); return;
    case '+': case '=': act(CMD_FASTER); return;
    case '-':           act(CMD_SLOWER); return;
    }
}

// Shapes is the app that forced `on_tick` to exist. The angle advances
// by ELAPSED time in 10 ms steps, not per call: a frame of textured
// teapot costs several steps under emulation, and a step per frame made
// the heavy scene spin slower than the light one. Capped, so a stall
// resumes where it was rather than jumping. It also runs the toolbar's
// tooltip clock.
#define STEP_NS 10000000ull
static int on_tick(struct uapp *a) {
    (void)a;
    static uint64_t next;
    uint64_t now = sys_monotonic_ns();
    if (!next) next = now;
    int steps = 0;
    while (next <= now && steps < 10) { next += STEP_NS; steps++; }
    if (next <= now) next = now + STEP_NS;   // the cap: drop the backlog
    g_angle += (fx_t)(g_speed * steps) * (FX_ONE / 1024);
    uui_toolbar_tick(&g_tb);
    return 1; // always repaint -- at speed 0 that redraws the same
              // pixels, which is exactly what "it stops dead" means
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a;
    g_w = w;
    g_h = h;
    layout();
}

// The first size, and the smallest, both measured from the font: wide
// enough for the whole toolbar, and never narrower than the scenes and
// the speed controls plus the View button.
static void on_size(int *w, int *h) {
    uui_toolbar_init(&g_tb, TOOLBAR, TOOLBAR_N);
    g_tb.item_flags = item_flags;
    g_tb.overflow = 1;
    g_tb.accent_latch = 1;
    g_tb.more.icon = 0;
    g_tb.more.tip = 0;
    g_tb.more.label = "View";
    // Measured with the readout at its WIDEST: it is re-measured every
    // draw, and a minimum fitted to "speed 3" folds + at speed 10.
    set_speed(40);

    int tb_w = 0, tb_h = 0;
    uui_toolbar_natural_size(&g_tb, &tb_w, &tb_h);
    int min_w = tb_w;
    for (g_tb.w = 120; g_tb.w < tb_w; g_tb.w += 4) {
        if (uui_toolbar_shown(&g_tb) >= KEEP_SHOWN) { min_w = g_tb.w; break; }
    }
    g_desc.min_w = min_w;
    set_speed(3);
    g_desc.min_h = tb_h + 2 * MARGIN + 160;

    g_w = tb_w > 560 ? tb_w : 560;
    g_h = tb_h + 2 * MARGIN + 380;
    *w = g_w;
    *h = g_h;
}

static void on_open(struct uapp *a) {
    (void)a;
    ugfx_texture_checker(g_tex_px, TEX_SIZE, TEX_SIZE, 8,
                         ugfx_rgb(235, 235, 240), ugfx_rgb(60, 80, 130));
    teapot_build(&g_pot);
    layout();

    ulog("gfxdemo: ready\n");
    ulog("gfxdemo: aa on\n");
    ulog("gfxdemo: shaded off\n");
    log_speed();
    log_scene();
}

int main(void) {
    g_desc = (struct uapp_desc){
        .title     = "Shapes",
        .app_id    = "gfxdemo",
        .on_size   = on_size,
        .flags     = UAPP_RESIZABLE,
        .widgets   = g_items,
        .widget_count = (int)(sizeof g_items / sizeof g_items[0]),
        .on_widget = on_widget,
        .on_open   = on_open,
        .on_draw   = on_draw,
        .on_tick   = on_tick,
        // A frame per timer tick when the machine keeps up; the angle
        // follows the clock either way (on_tick).
        .tick_ms   = 10,
        .on_key    = on_key,
        .on_resize = on_resize,
    };
    int rc = uapp_run(&g_desc);
    ulog("gfxdemo: exiting\n");
    return rc;
}
