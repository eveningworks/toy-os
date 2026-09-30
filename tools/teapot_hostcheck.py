#!/usr/bin/env python3
"""Check Shapes' teapot mesh (userland/shapes/teapot.c) and the depth-
tested triangle it is drawn with (userland/ui/ugfx_tex.c's ugfx_tri3d),
with the host gcc.

THE MESH, against a floating-point evaluation of the SAME control points
(parsed out of teapot.c, so the two cannot be handed different data):

  - every vertex lands within 0.002 units of the float Bezier surface,
    and the bounding radius agrees -- the Q16.16 evaluation is right
  - normals checked INDEPENDENTLY of how the mesh computes one: the body's point
    away from the teapot's axis, the lid's top points up, the bottom's
    points down -- so a normal flipped in both would still fail here
  - the winding claim teapot.h makes: a cell triangle whose normal faces
    the eye has a POSITIVE screen-space signed area (x right, y down),
    which is the culling test Shapes relies on

THE RASTERISER, on a small surface:

  - two triangles that PIERCE each other, drawn in either order through
    a depth buffer, give the same pixels bar the crossing line itself
    (equal depth, first drawn wins) -- and differ without one,
    which proves the scene really overlaps
  - Gouraud: a pixel beside each corner carries about that corner's shade

    python3 tools/teapot_hostcheck.py
    python3 tools/teapot_hostcheck.py --positive-control   # must FAIL

The control breaks each checked property in turn -- the mirror's
winding fix-up, teapot.h's triangle winding, the depth test -- and
requires the finding AIMED at it, not merely some finding. Needs only
gcc and the standard library.
"""

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MESH_DRIVER = r"""
#include "shapes/teapot.h"
#include <stdio.h>
static struct teapot t;
int main(void) {
    teapot_build(&t);
    printf("R %d\n", t.radius);
    for (int i = 0; i < TEAPOT_VERTS; i++)
        printf("%d %d %d %d %d %d\n", t.pos[i].x, t.pos[i].y, t.pos[i].z,
               t.nrm[i].x, t.nrm[i].y, t.nrm[i].z);
    // The triangles as teapot.h names them -- the claim under test is
    // about THOSE, so this does not keep its own copy of the list.
    for (int p = 0; p < TEAPOT_PATCHES; p++)
        for (int i = 0; i < TEAPOT_DIV; i++)
            for (int j = 0; j < TEAPOT_DIV; j++) {
                int v[6];
                teapot_cell_tris(p, i, j, v);
                printf("T %d %d %d\nT %d %d %d\n", v[0], v[1], v[2], v[3], v[4], v[5]);
            }
    return 0;
}
"""

# Draws two piercing triangles in the order argv[1] names ("ab" or
# "ba"), with a depth buffer unless argv[2] is "nozb", then a Gouraud
# triangle, and prints the surface as hex rows.
RASTER_DRIVER = r"""
#include "ui/ugfx_tex.h"
#include <stdio.h>
#include <string.h>
void ugfx_mark_dirty_rect(struct ugfx_surface *s, int x, int y, int w, int h) {
    (void)s; (void)x; (void)y; (void)w; (void)h;
}
#define W 64
#define H 64
static uint32_t px[W * H], depth[W * H];
int main(int argc, char **argv) {
    struct ugfx_surface s;
    memset(&s, 0, sizeof s);
    s.pixels = px; s.w = W; s.h = H;
    struct ugfx_zbuffer zb = { depth, 0, 0, W, H };
    int use_zb = !(argc > 2 && strcmp(argv[2], "nozb") == 0);
    // A leans back left to right, B the other way: they cross at x ~ 32.
    struct ugfx_texvert a[3] = { { 4, 4, 100, 0, 0, 255 }, { 60, 8, 300, 0, 0, 255 },
                                 { 8, 60, 100, 0, 0, 255 } };
    struct ugfx_texvert b[3] = { { 4, 10, 300, 0, 0, 255 }, { 60, 4, 100, 0, 0, 255 },
                                 { 60, 60, 100, 0, 0, 255 } };
    ugfx_zbuffer_clear(&zb);
    for (int pass = 0; pass < 2; pass++) {
        int first_a = argv[1][pass] == 'a';
        struct ugfx_texvert *v = first_a ? a : b;
        ugfx_tri3d(&s, use_zb ? &zb : 0, 0, first_a ? 0xff0000u : 0x0000ffu,
                   &v[0], &v[1], &v[2]);
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) printf("%06x ", px[y * W + x] & 0xffffff);
        printf("\n");
    }
    // Gouraud: corners at shade 0, 128, 255, over white, no depth.
    memset(px, 0, sizeof px);
    struct ugfx_texvert g[3] = { { 2, 2, 50, 0, 0, 0 }, { 61, 2, 50, 0, 0, 128 },
                                 { 2, 61, 50, 0, 0, 255 } };
    ugfx_tri3d(&s, 0, 0, 0xffffffu, &g[0], &g[1], &g[2]);
    printf("G %d %d %d\n", px[3 * W + 3] & 0xff, px[3 * W + 58] & 0xff, px[58 * W + 3] & 0xff);
    return 0;
}
"""


# --positive-control's poisons, run ONE AT A TIME: each breaks one
# thing, and must be caught by the finding aimed at it -- not merely by
# some finding, which a vacuous check would still let through.
POISONS = {
    # The mirror's winding fix-up: mirrored patches face inward.
    "mirror": ("userland/shapes/teapot.c", "int col = sx * sy < 0 ? 3 - k : k;",
               "int col = k;", "point INTO"),
    # teapot.h's triangles wound the other way.
    "winding": ("userland/shapes/teapot.h", "out[0] = a; out[1] = c; out[2] = d;",
                "out[0] = a; out[1] = d; out[2] = c;", "wind against"),
    # No depth test.
    "depth": ("userland/ui/ugfx_tex.c", "if (d <= zrow[x - zx]) continue;",
              "(void)d;", "draw order changed"),
}


def stage(tmp, poison=None):
    """Copy what the two drivers include into `tmp`, laid out as the
    includes spell it. Copied rather than reached with -I into
    kernel/include/api, which also holds the toolkit's own string.h."""
    files = {
        "fixed.h": "kernel/include/api/fixed.h",
        "geom.h": "kernel/include/api/geom.h",
        "win_proto.h": "kernel/include/abi/win_proto.h",
        "ui/ugfx.h": "userland/ui/ugfx.h",
        "ui/ugfx_tex.h": "userland/ui/ugfx_tex.h",
        "shapes/teapot.h": "userland/shapes/teapot.h",
        "teapot.c": "userland/shapes/teapot.c",
        "ugfx_tex.c": "userland/ui/ugfx_tex.c",
    }
    for dst, src in files.items():
        os.makedirs(os.path.join(tmp, os.path.dirname(dst)), exist_ok=True)
        body = open(os.path.join(ROOT, src)).read()
        if poison and POISONS[poison][0] == src:
            needle, repl = POISONS[poison][1], POISONS[poison][2]
            if needle not in body:
                sys.exit(f"teapot_hostcheck: poison {poison!r} cannot find its "
                         "target -- the code moved, fix the control")
            body = body.replace(needle, repl, 1)
        with open(os.path.join(tmp, dst), "w") as f:
            f.write(body)
    return open(os.path.join(tmp, "teapot.c")).read()


def compile_exe(tmp, name, sources):
    exe = os.path.join(tmp, name)
    cmd = ["gcc", "-O2", "-Wall", "-Wextra", "-Werror", "-I", tmp, "-o", exe] + \
          [os.path.join(tmp, s) for s in sources]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"teapot_hostcheck: compile of {name} failed:\n" + r.stderr)
    return exe


def parse_tables(src):
    """CP (hundred-thousandths) and PATCH, straight out of teapot.c."""
    cp_body = re.search(r"CP\[\d+\]\[3\] = \{(.*?)\n\};", src, re.S).group(1)
    cp = [tuple(int(v) / 100000.0 for v in m.groups())
          for m in re.finditer(r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", cp_body)]
    pa_body = re.search(r"PATCH\[\d+\]\[16\] = \{(.*?)\n\};", src, re.S).group(1)
    patches = [[int(v) for v in m.group(1).split(",")]
               for m in re.finditer(r"\{([^{}]*)\}", pa_body)]
    div = int(re.search(r"#define TEAPOT_DIV\s+(\d+)",
                        open(os.path.join(ROOT, "userland/shapes/teapot.h")).read()).group(1))
    return cp, patches, div


def bern(t):
    s = 1 - t
    return [s * s * s, 3 * t * s * s, 3 * t * t * s, t * t * t]


def reference(cp, patches, div):
    """The float surface, in the mesh's order and frame (see teapot.c)."""
    mirrors4 = [(1, 1), (-1, 1), (1, -1), (-1, -1)]
    out = []
    for src, pat in enumerate(patches):
        for sx, sy in (mirrors4 if src < 6 else [(1, 1), (1, -1)]):
            P = [[None] * 4 for _ in range(4)]
            for r in range(4):
                for k in range(4):
                    col = 3 - k if sx * sy < 0 else k
                    x, y, z = cp[pat[r * 4 + col]]
                    P[r][k] = (x * sx, y * sy, z)
            for i in range(div + 1):
                for j in range(div + 1):
                    u, v = i / div, j / div
                    bu, bv = bern(u), bern(v)
                    p = [sum(bu[r] * bv[k] * P[r][k][c] for r in range(4) for k in range(4))
                         for c in range(3)]
                    out.append((p[0], -p[2], p[1]))   # Newell's z-up to geom's y-down
    return out


def run_mesh(exe):
    lines = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split("\n")
    radius = int(lines[0].split()[1]) / 65536.0
    verts, tris = [], []
    for line in lines[1:]:
        if line.startswith("T "):
            tris.append(tuple(int(x) for x in line.split()[1:]))
        elif line.strip():
            v = [int(x) / 65536.0 for x in line.split()]
            verts.append((tuple(v[:3]), tuple(v[3:])))
    return radius, verts, tris


def check_mesh(radius, verts, tris, ref, div, fails):
    grid = div + 1
    # Centre the reference on its own bounding box, as build() does.
    lo = [min(p[c] for p in ref) for c in range(3)]
    hi = [max(p[c] for p in ref) for c in range(3)]
    ctr = [(lo[c] + hi[c]) / 2 for c in range(3)]
    ref = [tuple(p[c] - ctr[c] for c in range(3)) for p in ref]
    if len(ref) != len(verts):
        fails.append(f"{len(verts)} vertices, the reference has {len(ref)}")
        return

    worst = max(max(abs(verts[i][0][c] - ref[i][c]) for c in range(3)) for i in range(len(ref)))
    if worst > 0.002:
        fails.append(f"a vertex is {worst:.4f} units off the float surface")
    rr = max(math.sqrt(sum(c * c for c in p)) for p in ref)
    if abs(rr - radius) > 0.002:
        fails.append(f"radius {radius:.4f}, the float surface's is {rr:.4f}")

    def unit(n):
        m = math.sqrt(sum(c * c for c in n))
        return tuple(c / m for c in n) if m else (0.0, 0.0, 0.0)

    # Independent of any cross product: where the surface faces is a
    # property of the teapot's shape.
    per_patch = grid * grid
    bad_body = bad_lid = bad_bottom = 0
    for i, (p, n) in enumerate(verts):
        patch = i // per_patch
        n = unit(n)
        # Body (patches 4..11): away from the vertical (y) axis.
        if 4 <= patch < 12 and n[0] * p[0] + n[2] * p[2] <= 0:
            bad_body += 1
        # Bottom (20..23): down, which is +y here -- or level, on its
        # outer ring, where the surface turns vertical into the body.
        if 20 <= patch < 24 and n[1] < -0.05:
            bad_bottom += 1
    top = min(range(len(verts)), key=lambda i: verts[i][0][1])
    if unit(verts[top][1])[1] >= -0.9:
        bad_lid = 1
    if bad_body:
        fails.append(f"{bad_body} body normals point INTO the teapot")
    if bad_bottom:
        fails.append(f"{bad_bottom} bottom normals point up")
    if bad_lid:
        fails.append(f"the lid's top normal is {verts[top][1]}, not up")

    # teapot.h's winding claim, under an orthographic look down +z.
    wrong = checked = 0
    for tri in tris:
        P = [verts[k][0] for k in tri]
        area = ((P[1][0] - P[0][0]) * (P[2][1] - P[0][1]) -
                (P[2][0] - P[0][0]) * (P[1][1] - P[0][1]))
        nz = sum(unit(verts[k][1])[2] for k in tri) / 3
        if abs(nz) < 0.5 or abs(area) < 1e-4:
            continue    # edge-on: the sign means nothing
        checked += 1
        if (area > 0) != (nz < 0):
            wrong += 1
    if checked < 500:
        fails.append(f"only {checked} triangles face clearly toward or away -- the check is vacuous")
    elif wrong:
        fails.append(f"{wrong} of {checked} triangles wind against their normal "
                     "(a front face would be culled)")


def run_raster(exe, order, zb=True):
    out = subprocess.run([exe, order] + ([] if zb else ["nozb"]),
                         capture_output=True, text=True, check=True).stdout.split("\n")
    rows = [line.split() for line in out if line and not line.startswith("G")]
    g = [int(v) for v in next(line for line in out if line.startswith("G")).split()[1:]]
    return rows, g


def check_raster(exe, fails):
    ab, g = run_raster(exe, "ab")
    ba, _ = run_raster(exe, "ba")
    # Where the two cross their depths are EQUAL and the first drawn
    # keeps the pixel, so the crossing line itself may differ: a pixel a
    # row at most. A missing depth test changes hundreds.
    n = sum(x != y for ra, rb in zip(ab, ba) for x, y in zip(ra, rb))
    if n > len(ab):
        fails.append(f"with a depth buffer, draw order changed {n} pixels")
    nab, _ = run_raster(exe, "ab", zb=False)
    nba, _ = run_raster(exe, "ba", zb=False)
    if nab == nba:
        fails.append("without a depth buffer the two orders agree -- the "
                     "triangles do not overlap, so the check above proves nothing")
    # A is nearer at the left, B at the right.
    red = sum(v == "ff0000" for row in ab for v in row[:24])
    blue = sum(v == "0000ff" for row in ab for v in row[40:])
    if red < 100 or blue < 100:
        fails.append(f"the nearer triangle does not win: {red} red on the left, "
                     f"{blue} blue on the right")
    want = (0, 128, 255)
    for got, w, name in zip(g, want, ("first", "second", "third")):
        if abs(got - w) > 12:
            fails.append(f"Gouraud: beside the {name} corner the shade is {got}, not ~{w}")


def check_all(poison=None):
    tmp = tempfile.mkdtemp(prefix="teapot_hostcheck.")
    try:
        src = stage(tmp, poison)
        with open(os.path.join(tmp, "mesh_driver.c"), "w") as f:
            f.write(MESH_DRIVER)
        with open(os.path.join(tmp, "raster_driver.c"), "w") as f:
            f.write(RASTER_DRIVER)
        mesh = compile_exe(tmp, "mesh", ["teapot.c", "mesh_driver.c"])
        raster = compile_exe(tmp, "raster", ["ugfx_tex.c", "raster_driver.c"])
        fails = []
        cp, patches, div = parse_tables(src)
        radius, verts, tris = run_mesh(mesh)
        check_mesh(radius, verts, tris, reference(cp, patches, div), div, fails)
        check_raster(raster, fails)
        return fails, len(verts), radius
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="break each checked property in turn; each must be caught")
    args = ap.parse_args()

    if args.positive_control:
        missed = 0
        for name, (_, _, _, want) in POISONS.items():
            fails, _, _ = check_all(name)
            hit = [f for f in fails if want in f]
            print(f"  {'caught' if hit else 'MISSED'}  {name}: "
                  f"{hit[0] if hit else f'no finding with {want!r} among {fails}'}")
            missed += not hit
        if missed:
            print("teapot_hostcheck: positive control PASSED where it must fail")
            return 1
        print("teapot_hostcheck: positive control FAILED as it must, every poison caught")
        return 0

    fails, nverts, radius = check_all()
    for f in fails:
        print("FAIL:", f)
    if fails:
        return 1
    print(f"teapot_hostcheck: PASS ({nverts} vertices, radius {radius:.3f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
