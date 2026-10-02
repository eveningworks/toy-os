#!/usr/bin/env python3
"""The crash report viewer (Crash Reports' viewer face, lib/ucrash.c): a
real crash, read back, its backtrace checked against the HOST's reading.

WHAT IT CHECKS, and what a broken version would still pass:

  - a Notepad killed by SIGSEGV leaves a report, and `open` on it opens
    the VIEWER (Handles=.crash), not Notepad;
  - the viewer's frames, as it logs them, are checked one by one against
    this tool's OWN reading: the report's raw header (from `crashlog`,
    whose header is the kernel's text, not the library's), and
    `readelf` over the same build's binaries. Frame 0 is the header's
    rip; every frame's function and offset is what the host's symbol
    table says; and every frame after 0 FOLLOWS A CALL INSTRUCTION in
    the host's copy of the file -- so a scan that accepted any code
    address fails here even when its names are right;
  - the chain is the one a Notepad blocked in its event loop must have:
    uapp_run and main among the frames;
  - the document is DRAWN (ink in the report's rect), and `crashlog`
    prints the same frames;
  - Crash Reports' list opens the viewer with Enter.

Positive control: mutate.py making ucrash.c's after_call() return 1
reddens the call check (stale return addresses get in).
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results                      # noqa: E402

SEED = os.path.join(REPO, "seed", "sync")
_res = Results()
check = _res.check


def readelf_segments(elf):
    out = subprocess.run(["readelf", "-lW", elf], capture_output=True, text=True).stdout
    segs = []
    for ln in out.splitlines():
        m = re.match(r"\s+LOAD\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+0x[0-9a-f]+\s+([RWE ]+)", ln)
        if m:
            segs.append((int(m.group(2), 16), int(m.group(1), 16), int(m.group(3), 16), "E" in m.group(4)))
    return segs


def readelf_funcs(elf):
    out = subprocess.run(["readelf", "-sW", elf], capture_output=True, text=True).stdout
    funcs = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) >= 8 and p[3] == "FUNC" and p[6] != "UND" and int(p[1], 16):
            funcs.append((int(p[1], 16), int(p[2]), p[7]))
    return funcs


class Host:
    """The host's own reading of one binary."""

    def __init__(self, path):
        self.elf = SEED + path
        self.segs = readelf_segments(self.elf)
        self.funcs = readelf_funcs(self.elf)
        self.raw = open(self.elf, "rb").read()
        self.base = min(s[0] for s in self.segs) & ~0xFFF

    def name(self, va):
        best = None
        for v, size, n in self.funcs:
            if v <= va < v + max(size, 1) and (best is None or v > best[0]):
                best = (v, n)
        return (best[1], va - best[0]) if best else (None, None)

    def after_call(self, va):
        seg = next((s for s in self.segs if s[0] <= va < s[0] + s[2]), None)
        if not seg:
            return False
        off = va - seg[0] + seg[1]
        c = self.raw[off - 7:off]
        if len(c) < 7:
            return False
        if c[2] == 0xE8:
            return True
        return any(c[7 - b] == 0xFF and (c[8 - b] >> 3) & 7 == 2 for b in (2, 3, 6, 7))


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "crashview_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("Crash report viewer (lib/ucrash.c, crashreports' viewer)")

    dbg.spawn("/bin/wm/apps/notepad", "Notepad")
    time.sleep(2)
    ps = dbg.send("sh ps") or ""
    pid = next((ln.split()[0] for ln in ps.splitlines() if ln.strip().endswith("notepad")), None)
    if not check("a Notepad is running to crash", pid):
        return _res.finish("crashview_test")
    dbg.send(f"sh kill -SEGV {pid}")
    time.sleep(3)
    report = f"/var/crash/notepad-{pid}.crash"
    text = dbg.send(f"sh crashlog {report}") or ""
    head = text[text.find("toy-os crash report"):] if "toy-os crash report" in text else ""
    if not check("the kill left a report", head, text.strip()[-120:]):
        return _res.finish("crashview_test")
    rip = int(re.search(r"^rip: 0x([0-9a-f]+)", head, re.M).group(1), 16)
    maps = [(int(a, 16), int(b, 16), path or None, kind)
            for kind, a, b, path in re.findall(r"^map: (\w+) 0x([0-9a-f]+)-0x([0-9a-f]+)(?: prot \d+)? ?(.*)$",
                                               head, re.M)]
    program = re.search(r"^program: (.*)$", head, re.M).group(1)

    # A dialog from the desktop's crash notice may be up; the viewer is
    # what `open` gives a .crash file.
    dbg.logs("crashreports:", clear=True)
    dbg.send(f"sh open {report}")
    time.sleep(3)
    title = f"notepad-{pid}.crash - Crash Report"
    win = next((w for w in dbg.windows() if w.get("title") == title), None)
    check("`open` on a .crash file opens the viewer", win is not None,
          f"windows {[w.get('title') for w in dbg.windows()]}")
    # Its log went to `sh`'s console; the one started by the desktop logs
    # where this tool reads.
    if win:
        dbg.send(f"gui close {win['z']}")
        time.sleep(1)
    dbg.logs("crashreports:", clear=True)
    win = dbg.spawn(f"/bin/wm/apps/crashreports {report}", title)
    time.sleep(2)
    frames = []
    for ln in dbg.logs("crashreports: frame", clear=False):
        m = re.search(r"frame (\d+) 0x([0-9a-f]+) (\S+) (\S+) \+0x([0-9a-f]+)", ln)
        if m:
            frames.append((int(m.group(1)), int(m.group(2), 16), m.group(3), m.group(4), int(m.group(5), 16)))
    if not check("the viewer read a backtrace", len(frames) >= 3, f"{len(frames)} frames"):
        return _res.finish("crashview_test")
    check("frame 0 is the report's rip", frames[0][1] == rip, f"{frames[0][1]:#x} vs {rip:#x}")

    hosts = {}
    wrong, nocall = [], []
    for n, addr, module, func, off in frames:
        hit = next((m for m in maps if m[0] <= addr < m[1] and m[3] in ("image", "file")), None)
        path = program if hit and hit[3] == "image" else hit[2] if hit else None
        if not path:
            wrong.append(f"#{n} {addr:#x} in no mapping")
            continue
        h = hosts.setdefault(path, Host(path))
        lowest = min(m[0] for m in maps if (m[2] == path) or (m[3] == "image" and path == program))
        va = addr - (lowest - h.base)
        want, woff = h.name(va)
        if (want or "-") != func or (want and woff != off):
            wrong.append(f"#{n} {func}+{off:#x}, host says {want}+{woff if woff is not None else 0:#x}")
        if n > 0 and not h.after_call(va):
            nocall.append(f"#{n} {func}+{off:#x}")
    check("every frame's name and offset is the host's symbol table's", not wrong, "; ".join(wrong)[:200])
    check("every frame after 0 follows a call instruction", not nocall, "; ".join(nocall)[:200])
    names = [f[3] for f in frames]
    check("the chain is a blocked event loop's: uapp_run, then main",
          "uapp_run" in names and "main" in names and names.index("uapp_run") < names.index("main"),
          " <- ".join(names))
    check("crashlog prints the same frames",
          all(f"{f[1]:#x}" in text for f in frames), text.split("toy-os crash report")[0].strip()[-200:])

    w = dbg.widgets(title).get("report")
    if check("the viewer reports its document", w is not None):
        from PIL import Image
        shot = os.path.join(args.tmp, f"crashview_{os.getpid()}.png")
        dbg.settle()
        qmp.screenshot(shot)
        im = Image.open(shot).convert("RGB")
        x, y = w["screen"]["x"], w["screen"]["y"]
        ink = sum(1 for yy in range(y, y + w["h"], 2) for xx in range(x, x + w["w"], 3)
                  if sum(im.getpixel((xx, yy))) < 200)
        check("...and draws it", ink > 400, f"{ink} dark samples")
    if win:
        dbg.send(f"gui close {win['z']}")
        time.sleep(1)

    # The list, Enter on the newest report.
    dbg.logs("crashreports:", clear=True)
    lw = dbg.spawn("/bin/wm/apps/crashreports", "Crash Reports")
    time.sleep(1)
    dbg.key("0x0d")
    time.sleep(3)
    opened = any(w.get("title", "").endswith(" - Crash Report") for w in dbg.windows())
    check("Enter in Crash Reports opens the viewer", opened, f"windows {[w.get('title') for w in dbg.windows()]}")
    for w in dbg.windows():
        if w.get("title", "").endswith("Crash Report") or w.get("title") in ("Crash Reports", lw and lw.get("title")):
            dbg.send(f"gui close {w['z']}")
    return _res.finish("crashview_test")


if __name__ == "__main__":
    sys.exit(main())
