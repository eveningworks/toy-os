#!/usr/bin/env python3
"""lib/utrash through /bin/trash: the Recycle Bin, checked as text.

1. **`trash put` moves a file out of its folder and into the bin**, and
   `trash list` names where it came from.
2. **A second item with the same name gets a numbered one** (`x.2.txt`),
   the extension kept -- two deletes must never overwrite each other.
3. **The info file is the freedesktop format**: `[Trash Info]`, a
   percent-encoded `Path=` (a `+` in the name is `%2B`) and a
   `DeletionDate=`. A Linux desktop reading the disk depends on this.
4. **`trash restore` puts it back with its CONTENT** -- the size, not just
   the name, since an empty file would pass a listing check.
5. **A restore onto a name that is taken is refused**, and the item stays
   in the bin.
6. **A folder goes in and comes back whole**, its contents included.
7. **A RAM volume has no bin**: `trash put` on /tmp refuses and leaves the
   file alone.
8. **`trash empty` empties every bin.**

RUN IT ON DEMAND against a throwaway copy of disk.img; it works under
/tt and in the bin, and empties the bin at the end.
"""
import argparse
import os
import subprocess
import sys
from harness import Results  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ROOT = "/tt"
BIN = "/home/.Trash"


def run(cmd, instance=None):
    argv = [sys.executable, os.path.join(HERE, "vm.py")]
    if instance is not None:
        argv += ["--instance", str(instance)]
    argv += ["exec", cmd]
    out = subprocess.run(argv, cwd=REPO, capture_output=True, text=True).stdout
    keep = []
    for line in out.splitlines():
        s = line.strip()
        if not s or s.startswith(("vm:", "syscall:", "elf_run:", "init:", "---",
                                   "wm:", "win_surface:", "cursor:", "mouse:")):
            continue
        keep.append(s)
    return keep


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--instance", type=int, default=None)
    args = ap.parse_args()
    inst = args.instance
    res = Results()

    def sh(cmd):
        return run(cmd, inst)

    def names(path):
        return sorted(n.rstrip("/") for n in sh(f"ls {path}") if ":" not in n and "exit" not in n)

    def size(directory, name):
        for line in sh(f"ls -l {directory}"):
            parts = line.split()
            if len(parts) >= 5 and parts[0] == "-" and parts[-1] == name:
                return int(parts[1])
        return -1

    def listed():
        return [ln for ln in sh("trash list") if "exit" not in ln]

    sh("trash empty")
    sh(f"rm -r {ROOT}")
    sh(f"mkdir {ROOT}")
    sh(f"mkdir {ROOT}/two")
    sh(f"cp /etc/timezones {ROOT}/a+b.txt")
    sh(f"cp /etc/resolv.conf {ROOT}/two/a+b.txt")
    want = size(ROOT, "a+b.txt")

    # 1. put
    sh(f"trash put {ROOT}/a+b.txt")
    rows = listed()
    res.check("trash put takes the file out of its folder",
              "a+b.txt" not in names(ROOT), f"root={names(ROOT)}")
    res.check("...and trash list names where it came from",
              any(r.split()[0] == "a+b.txt" and r.endswith(f"{ROOT}/a+b.txt") for r in rows), repr(rows))

    # 2. the same name again
    out = " ".join(sh(f"trash put {ROOT}/two/a+b.txt"))
    res.check("a second item of the same name gets a numbered one, extension kept",
              "a+b.2.txt" in out and "a+b.2.txt" in names(f"{BIN}/files"),
              f"put said {out!r}, files={names(BIN + '/files')}")

    # 3. the info file
    info = sh(f"cat {BIN}/info/a+b.txt.trashinfo")
    res.check("the info file is freedesktop's: header, %-encoded Path=, DeletionDate=",
              info[:1] == ["[Trash Info]"] and f"Path={ROOT}/a%2Bb.txt" in info and
              any(i.startswith("DeletionDate=20") and "T" in i for i in info), repr(info))

    # 4. restore, with content
    out = " ".join(sh("trash restore a+b.txt"))
    res.check("trash restore puts it back with its content",
              size(ROOT, "a+b.txt") == want and want > 0, f"said {out!r}, {want} -> {size(ROOT, 'a+b.txt')} bytes")
    res.check("...and it is no longer in the bin",
              not any(r.split()[0] == "a+b.txt" for r in listed()), repr(listed()))

    # 5. restore onto a taken name
    sh(f"cp /etc/timezones {ROOT}/two/a+b.txt")
    out = " ".join(sh("trash restore a+b.2.txt"))
    res.check("a restore onto a taken name is refused, and the item stays in the bin",
              "in the way" in out and any(r.split()[0] == "a+b.2.txt" for r in listed()), repr(out))

    # 6. a folder, whole
    sh(f"mkdir {ROOT}/dir")
    sh(f"cp /etc/timezones {ROOT}/dir/inside.txt")
    sh(f"trash put {ROOT}/dir")
    gone = "dir" not in names(ROOT)
    sh("trash restore dir")
    res.check("a folder goes into the bin and comes back with its contents",
              gone and names(f"{ROOT}/dir") == ["inside.txt"], f"gone={gone} dir={names(ROOT + '/dir')}")

    # 7. a RAM volume has no bin
    sh("cp /etc/timezones /tmp/tt-ram.txt")
    out = " ".join(sh("trash put /tmp/tt-ram.txt"))
    res.check("a file on a RAM volume is refused (no bin there) and left alone",
              "no bin" in out and "tt-ram.txt" in names("/tmp"), repr(out))
    sh("rm /tmp/tt-ram.txt")

    # 8. empty
    sh(f"trash put {ROOT}/two")
    sh("trash empty")
    res.check("trash empty leaves every bin empty",
              listed() == ["the Recycle Bin is empty"] and names(f"{BIN}/files") == [],
              f"list={listed()} files={names(BIN + '/files')}")

    sh(f"rm -r {ROOT}")
    print(f"\ntrash_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
