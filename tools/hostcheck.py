"""The build half every `*_hostcheck.py` had written for itself.

A host check compiles toy-os source with the HOST's gcc beside a small C
driver, and compares what it computes against a foreign implementation.
Its positive control patches a COPY of the source, never the checked-in
file. Those three steps -- write the driver, stage the sources with the
control's edits, compile -- are the part that did not differ between the
tools; what they compare and how stays in each tool.

    import hostcheck
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    src = hostcheck.stage(tmp, "userland/lib/utween.c",
                          edits=[(CURVE, LINEAR)], apply=args.positive_control,
                          tool="utween_hostcheck")
    exe = hostcheck.compile(tmp, "utween_host", [src, drv], includes=[tmp])

A control's `before` text that is no longer in the source STOPS the run:
a control that silently patched nothing would report a working check as
able to fail when it had never been given the chance.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STRICT = ("-O2", "-Wall", "-Wextra", "-Werror")


def write(tmp, name, text):
    path = os.path.join(tmp, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(text)
    return path


def stage(tmp, rel, dest=None, edits=(), apply=False, tool="hostcheck"):
    """Copy the repo file `rel` into `tmp` (as `dest`, default its base
    name), applying each (before, after) edit once when `apply`."""
    dst = os.path.join(tmp, dest or os.path.basename(rel))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not apply or not edits:
        shutil.copy(os.path.join(ROOT, rel), dst)
        return dst
    with open(os.path.join(ROOT, rel)) as fh:
        body = fh.read()
    for before, after in edits:
        if before not in body:
            sys.exit(f"{tool}: the positive control cannot find {before!r} in {rel} "
                     "-- the code moved, fix the control")
        body = body.replace(before, after, 1)
    with open(dst, "w") as fh:
        fh.write(body)
    return dst


def compile(tmp, name, sources, flags=STRICT, includes=(), tool="hostcheck"):
    """gcc the sources into tmp/name; stops with gcc's errors on failure."""
    exe = os.path.join(tmp, name)
    cmd = ["gcc", *flags, *(f"-I{i}" for i in includes), "-o", exe, *sources]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{tool}: compile of {name} failed:\n{r.stderr}")
    return exe
