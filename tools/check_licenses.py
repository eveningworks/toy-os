#!/usr/bin/env python3
"""Every vendored port, shipped font, palette and notice file is named in LICENSE.

WHY THIS EXISTS
---------------
`LICENSE` carries an inventory of the third-party material in this
repository. An inventory maintained by somebody remembering is the
shape of every maintenance burden this project has deleted -- and it
had already drifted twice by the time this check was written:

  - `userland/ports/doom/` is GPL-2-or-later source, vendored into an
    MIT repository, and LICENSE did not mention it at ALL.
  - LICENSE described "two complete third-party font files". There were
    five. `vera-mono.ttf` and both bold faces were unlisted, and Vera
    is a third license family with its own notice file.

Neither is a licensing violation on its own -- the per-file notices
were all present and correct, which is what the licenses actually
require. What was wrong is that a reader of LICENSE could not learn
that GPL code is in the tree. This makes that a build property.

WHAT IT CHECKS
  1. Every directory under userland/ports/ has a license file.
  2. ...and is named in LICENSE.
  3. Every font under data/fonts/ has a license file beside it.
  4. ...and is named in LICENSE.
  5. Every license file those fonts point at actually exists.
  6. Every Terminal colour scheme (data/usr/share/terminal/*.scheme) is
     named in LICENSE -- as another project's palette, or in the list of
     original ones. A palette is data, not code, which is how three of
     them shipped unrecorded until 2026-10-02: naming EVERY scheme forces
     the question for each new one, where a check for third-party ones
     alone would need to know which those are.
  7. Every notice file under data/licenses/ is named in LICENSE by path,
     and every data/licenses/ path LICENSE names exists.
  8. Every port declares its licence in the Makefile (PORT_LICENSE_<name>,
     SPDX ids joined by OR -- what NOGPL=1 decides by), each family it
     names is the one its licence file is written in, a file carrying
     the GPL's preamble is not declared as something else, and a
     GPL-only port names the programs built from it (PORT_PROGRAMS_<name>).
  9. The Makefile copies each port's licence file onto the image
     (/usr/share/licenses/<port>.txt) by the SAME list of file names
     check 1 accepts -- a name only one of the two knew would pass here
     and ship no notice.

WHAT IT DOES NOT CHECK, and cannot: whether the license NAMED is the
license the code is actually under, beyond its family (check 8). Nothing static can read a
directory and know it is really GPL-2-or-later rather than GPL-3. That
part is a human reading the vendored LICENSE file, and it is why the
entries above quote the version language rather than paraphrasing it.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

PORTS = os.path.join(REPO, "userland", "ports")
FONTS = os.path.join(REPO, "data", "fonts")
SCHEMES = os.path.join(REPO, "data", "usr", "share", "terminal")
NOTICES = os.path.join(REPO, "data", "licenses")

LICENSE_FILENAMES = ("LICENSE", "LICENSE.txt", "LICENSE.md", "COPYING", "COPYING.txt")

# An SPDX id's family, and words its licence text always contains.
FAMILIES = (
    ("AGPL-", "GNU AFFERO GENERAL PUBLIC LICENSE"),
    ("LGPL-", "GNU LESSER GENERAL PUBLIC LICENSE"),
    ("GPL-", "GNU GENERAL PUBLIC LICENSE"),
    ("Apache-", "Apache License"),
    ("MIT", "Permission is hereby granted"),
    ("BSD-", "Redistribution and use in source and binary forms"),
)


def family(spdx_id):
    for prefix, marker in FAMILIES:
        if spdx_id.startswith(prefix):
            return prefix, marker
    return None, None


def port_declarations():
    """{port: (licence expression, programs)} as the Makefile declares them."""
    lic, progs = {}, {}
    for line in open(os.path.join(REPO, "Makefile")):
        m = re.match(r"PORT_LICENSE_(\w+)\s*=\s*(.*?)\s*$", line)
        if m:
            lic[m.group(1)] = m.group(2)
        m = re.match(r"PORT_PROGRAMS_(\w+)\s*=\s*(.*?)\s*$", line)
        if m:
            progs[m.group(1)] = m.group(2).split()
    return lic, progs


def check_declaration(name, d, expr, programs):
    """Problems with one port's PORT_LICENSE_ line, as strings."""
    if expr is None:
        return [f"userland/ports/{name}/ declares no licence -- add PORT_LICENSE_{name} "
                f"to the Makefile's NOGPL=1 block"]
    words = expr.split()
    ids = words[0::2]
    if not ids or any(w != "OR" for w in words[1::2]) or len(words) % 2 == 0:
        return [f"PORT_LICENSE_{name} = {expr!r}: only SPDX ids joined by OR are understood"]
    text = ""
    for f in LICENSE_FILENAMES:
        if os.path.exists(os.path.join(d, f)):
            text += open(os.path.join(d, f), errors="replace").read()
    problems = []
    for i in ids:
        prefix, marker = family(i)
        if prefix is None:
            problems.append(f"PORT_LICENSE_{name}: {i} is not a family this check knows")
        elif marker not in text:
            problems.append(f"PORT_LICENSE_{name} says {i}, but its licence file has no "
                            f"\"{marker}\"")
    gpl_text = "GNU GENERAL PUBLIC LICENSE" in text or "GNU AFFERO" in text
    if gpl_text and not any(i.startswith(("GPL-", "AGPL-")) for i in ids):
        problems.append(f"userland/ports/{name}/'s licence file is the GPL, and "
                        f"PORT_LICENSE_{name} does not say so -- NOGPL=1 would ship it")
    gpl_only = all(i.startswith(("GPL-", "AGPL-")) for i in ids)
    if gpl_only and not programs:
        problems.append(f"userland/ports/{name}/ is GPL-only and declares no "
                        f"PORT_PROGRAMS_{name} -- NOGPL=1 cannot leave its programs out")
    for prog in programs:
        if not os.path.exists(os.path.join(REPO, "userland", prog + ".c")):
            problems.append(f"PORT_PROGRAMS_{name} names {prog}, and there is no "
                            f"userland/{prog}.c")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    lic_path = os.path.join(REPO, "LICENSE")
    if not os.path.exists(lic_path):
        print("check_licenses: no LICENSE at the repository root")
        return 1
    lic = open(lic_path).read()

    problems, checked = [], 0
    decl_lic, decl_progs = port_declarations()

    # --- the notices the image carries (check 9) ----------------------
    mk = open(os.path.join(REPO, "Makefile")).read()
    m = re.search(r"addprefix userland/ports/\$\(p\)/,([^)]*)\)", mk)
    seeded = tuple(m.group(1).split()) if m else ()
    if seeded != LICENSE_FILENAMES:
        problems.append(f"the Makefile seeds port licences named {seeded or 'nothing'}, "
                        f"and this check accepts {LICENSE_FILENAMES} -- they must be one list")

    # --- vendored ports ------------------------------------------------
    if os.path.isdir(PORTS):
        for name in sorted(os.listdir(PORTS)):
            d = os.path.join(PORTS, name)
            if not os.path.isdir(d):
                continue
            checked += 1
            problems += check_declaration(name, d, decl_lic.get(name),
                                          decl_progs.get(name, []))
            if not any(os.path.exists(os.path.join(d, f)) for f in LICENSE_FILENAMES):
                problems.append(f"userland/ports/{name}/ has no license file -- a "
                                f"vendored port must keep the terms it arrived under")
            # Named by PATH, not by bare directory name: "doom" appears
            # in prose all over the file, and matching that would make
            # the check pass on a mention rather than on an entry.
            if f"userland/ports/{name}/" not in lic:
                problems.append(f"userland/ports/{name}/ is vendored but is not named "
                                f"in LICENSE -- a reader cannot learn what it is under")
            elif args.verbose:
                print(f"  ok    userland/ports/{name}/")

    # --- fonts ---------------------------------------------------------
    if os.path.isdir(FONTS):
        for name in sorted(os.listdir(FONTS)):
            if not name.lower().endswith((".ttf", ".otf")):
                continue
            checked += 1
            if name not in lic:
                problems.append(f"data/fonts/{name} ships but is not named in "
                                f"LICENSE's \"Third-party font\" section")
            elif args.verbose:
                print(f"  ok    data/fonts/{name}")
        # Every notice file the section points at must be real.
        for name in sorted(os.listdir(FONTS)):
            if name.startswith("LICENSE"):
                checked += 1
                if f"data/fonts/{name}" not in lic:
                    problems.append(f"data/fonts/{name} exists but LICENSE never "
                                    f"points at it")
        for line in lic.splitlines():
            for token in line.split():
                token = token.strip(".,;:")
                if token.startswith("data/fonts/LICENSE"):
                    if not os.path.exists(os.path.join(REPO, token)):
                        problems.append(f"LICENSE points at {token}, which does not exist")

    # --- Terminal colour schemes ------------------------------------------
    if os.path.isdir(SCHEMES):
        for name in sorted(os.listdir(SCHEMES)):
            if not name.endswith(".scheme"):
                continue
            checked += 1
            if name not in lic:
                problems.append(f"data/usr/share/terminal/{name} ships but LICENSE does not "
                                f"say whose palette it is -- name it under \"Third-party "
                                f"data\", or in the list of original schemes")
            elif args.verbose:
                print(f"  ok    data/usr/share/terminal/{name}")

    # --- notice files shipped for generated data -------------------------
    if os.path.isdir(NOTICES):
        for name in sorted(os.listdir(NOTICES)):
            checked += 1
            if f"data/licenses/{name}" not in lic:
                problems.append(f"data/licenses/{name} exists but LICENSE never points at it")
            elif args.verbose:
                print(f"  ok    data/licenses/{name}")
    for line in lic.splitlines():
        for token in line.split():
            token = token.strip(".,;:()")
            if token.startswith("data/licenses/") and not os.path.exists(os.path.join(REPO, token)):
                problems.append(f"LICENSE points at {token}, which does not exist")

    if problems:
        print(f"check_licenses: {len(problems)} problem(s)\n")
        for p in problems:
            print(f"  {p}")
        print("\nLICENSE carries the inventory of third-party material. Add the entry "
              "there\nrather than deleting the check -- see its \"Everything else\" "
              "section.")
        return 1

    print(f"check_licenses: ok -- {checked} vendored port(s), font(s), palette(s) and "
          f"notice file(s), all named in LICENSE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
