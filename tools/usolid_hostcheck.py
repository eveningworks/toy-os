#!/usr/bin/env python3
"""Check usolid_slice() (userland/lib/usolid.c) -- the Desktop cube's
damage -- with the host gcc.

A slice cuts a solid with a plane and caps the hole. What a broken one
would get wrong, each checked on the cube, the pyramid and the ball:

  - THE SOLID STAYS CLOSED AND WOUND ONE WAY: every directed edge has its
    reverse somewhere, matched by position. A missing cap, a cap wound
    backwards, or two faces meeting a rounding apart all leave an edge
    without its partner.
  - THE CUT IS WHERE IT WAS ASKED: right after each cut, the solid's
    reach along its direction is the plane -- or unchanged, where an
    earlier cut had already taken the solid back past it.
  - THE CAP SHOWS THE INSIDE: some triangles are textured wholly from
    the interior rows the caller named.

    python3 tools/usolid_hostcheck.py
    python3 tools/usolid_hostcheck.py --positive-control   # must FAIL

The control breaks each property in turn -- no cap, the cap wound the
wrong way, no welding of crossings, the plane's offset ignored -- and
requires the finding AIMED at it. Needs only gcc.
"""

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostcheck  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HARNESS = r'''
#include "lib/usolid.h"
#include <stdio.h>
#include <stdlib.h>
void ugfx_tri3d(struct ugfx_surface *a, struct ugfx_zbuffer *b, const struct ugfx_texture *c,
                uint32_t d, const struct ugfx_texvert *e, const struct ugfx_texvert *f,
                const struct ugfx_texvert *g) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; }
static struct usolid s;
static int open_edges(void) {
    int bad = 0;
    for (int t = 0; t < s.nt; t++) for (int k = 0; k < 3; k++) {
        struct geom_pt3 a = s.v[s.t[t][k]].p, b = s.v[s.t[t][(k + 1) % 3]].p;
        int found = 0;
        for (int u = 0; u < s.nt && !found; u++) for (int j = 0; j < 3; j++) {
            struct geom_pt3 c = s.v[s.t[u][j]].p, d = s.v[s.t[u][(j + 1) % 3]].p;
            if (labs(c.x - b.x) < 8 && labs(c.y - b.y) < 8 && labs(c.z - b.z) < 8 &&
                labs(d.x - a.x) < 8 && labs(d.y - a.y) < 8 && labs(d.z - a.z) < 8) { found = 1; break; }
        }
        if (!found) bad++;
    }
    return bad;
}
static int interior_tris(int iv) {
    int n = 0;
    for (int t = 0; t < s.nt; t++)
        n += s.v[s.t[t][0]].v >= iv && s.v[s.t[t][1]].v >= iv && s.v[s.t[t][2]].v >= iv;
    return n;
}
int main(void) {
    const char *names[] = { "cube", "pyramid", "ball" };
    struct geom_pt3 dirs[4] = { { FX_ONE, FX_ONE, FX_ONE }, { -FX_ONE, FX_ONE, -FX_ONE },
                                { FX_ONE, -FX_ONE / 3, 0 }, { 0, 0, -FX_ONE } };
    for (int sh = 0; sh < 3; sh++) {
        usolid_build(&s, sh, NULL, 512, 288);
        printf("%s uncut open %d\n", names[sh], open_edges());
        fx_t at[4], depth = FX_ONE * 3 / 10;
        for (int i = 0; i < 4; i++) { dirs[i] = usolid_unit(dirs[i]); at[i] = usolid_support(&s, dirs[i]); }
        // EACH CUT CHECKED AS IT IS MADE: a later one may trim past an
        // earlier one's plane. Where the solid reached past the plane, it
        // now stops at it; where it did not, it is untouched.
        int off = 0;
        for (int i = 0; i < 4; i++) {
            fx_t target = at[i] - depth, before = usolid_support(&s, dirs[i]);
            usolid_slice(&s, dirs[i], target, 288, 512);
            fx_t after = usolid_support(&s, dirs[i]);
            if (before > target + FX_ONE / 100 ? labs((long)(after - target)) > FX_ONE / 100 : after != before)
                off++;
        }
        printf("%s cut open %d misplaced %d interior %d nv %d nt %d\n", names[sh],
               open_edges(), off, interior_tris(288), s.nv, s.nt);
    }
    return 0;
}
'''

POISONS = {
    "no-cap": ("if (nr >= 3 && g_snv + nr + 1 <= USOLID_VERT_MAX) {",
               "if (0 && nr >= 3 && g_snv + nr + 1 <= USOLID_VERT_MAX) {", "open"),
    "cap-wound-wrong": ("if (facing(g_sv[ci].p, g_sv[a].p, g_sv[b].p, n) > 0) st_add(ci, b, a);",
                        "if (facing(g_sv[ci].p, g_sv[a].p, g_sv[b].p, n) <= 0) st_add(ci, b, a);", "open"),
    "no-weld": ("                g_sv[i].p = a;\n                break;", "                break;", "open"),
    "offset-ignored": ("side[i] = dot3(n, s->v[i].p) - off;",
                       "side[i] = dot3(n, s->v[i].p) - off + FX_ONE / 10;", "misplaced"),
}


def stage(tmp, poison=None):
    """What the harness includes, laid out as the includes spell it --
    copied rather than reached with -I into kernel/include/api, which also
    holds the toolkit's own string.h."""
    files = {
        "fixed.h": "kernel/include/api/fixed.h",
        "geom.h": "kernel/include/api/geom.h",
        "win_proto.h": "kernel/include/abi/win_proto.h",
        "ui/ugfx.h": "userland/ui/ugfx.h",
        "ui/ugfx_tex.h": "userland/ui/ugfx_tex.h",
        "lib/usolid.h": "userland/lib/usolid.h",
        "usolid.c": "userland/lib/usolid.c",
        "fixed.c": "kernel/lib/fixed.c",
        "geom.c": "kernel/lib/geom.c",
    }
    for dst, src in files.items():
        os.makedirs(os.path.join(tmp, os.path.dirname(dst)), exist_ok=True)
        body = open(os.path.join(ROOT, src)).read()
        if poison and dst == "usolid.c":
            needle, repl, _ = POISONS[poison]
            if needle not in body:
                sys.exit(f"usolid_hostcheck: poison {poison!r} cannot find its target -- "
                         "the code moved, fix the control")
            body = body.replace(needle, repl, 1)
        with open(os.path.join(tmp, dst), "w") as f:
            f.write(body)
    with open(os.path.join(tmp, "harness.c"), "w") as f:
        f.write(HARNESS)


def run(poison=None):
    with tempfile.TemporaryDirectory() as tmp:
        stage(tmp, poison)
        exe = hostcheck.compile(tmp, "slice", [os.path.join(tmp, s) for s in
                                               ("harness.c", "usolid.c", "fixed.c", "geom.c")],
                                flags=["-O1", "-w", "-include", "string.h"],
                                includes=[tmp], tool="usolid_hostcheck")
        out = subprocess.run([exe], capture_output=True, text=True, timeout=120).stdout
    findings = []
    for line in out.splitlines():
        f = line.split()
        if f[1] == "uncut" and int(f[3]):
            findings.append(("open", f"{f[0]}: {f[3]} open edges before any cut"))
        if f[1] == "cut":
            vals = dict(zip(f[2::2], f[3::2]))
            if int(vals["open"]):
                findings.append(("open", f"{f[0]}: {vals['open']} open edges after four cuts"))
            if int(vals["misplaced"]):
                findings.append(("misplaced", f"{f[0]}: {vals['misplaced']} cuts not at their depth"))
            if not int(vals["interior"]):
                findings.append(("interior", f"{f[0]}: no triangle shows the interior rows"))
    return out, findings


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    if args.positive_control:
        ok = True
        for name, (_, _, aim) in POISONS.items():
            _, findings = run(name)
            hit = any(k == aim for k, _ in findings)
            print(f"  {'FIRED' if hit else 'BLIND'}  {name}: {[m for _, m in findings][:2]}")
            ok &= hit
        print("usolid_hostcheck: positive control", "fired for every poison" if ok else "is BLIND")
        return 1 if ok else 0   # a control that fires is the command FAILING, as asked
    out, findings = run()
    print(out, end="")
    for _, m in findings:
        print(f"  FAIL  {m}")
    print(f"usolid_hostcheck: {'clean' if not findings else str(len(findings)) + ' finding(s)'}")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
