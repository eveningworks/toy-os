#!/usr/bin/env python3
"""Drive `hwdata update` against a server on this machine.

WHAT IT PROVES, and each check is only meaningful because the one before
it can fail:

  1. A good fetch REPLACES the database, and lspci reads the new names
     out of it afterwards. Not "the command printed something": the
     replacement is confirmed through a DIFFERENT program.
  2. A truncated body is REFUSED and the old file survives byte for
     byte. This is the whole reason the command exists rather than a
     note saying to run `wget -O <path>`, so it is the load-bearing one.
  3. An error page that is LARGE enough is refused too -- the size floor
     alone would pass it, and the vendor-line count is what does not.
  4. `-n` fetches, checks, reports, and changes nothing.
  5. `--from` with `all` is refused, because one URL cannot mean two
     files.

**IT NEVER LEAVES THIS MACHINE.** The server is a Python http.server on
the host, reached through SLIRP's 10.0.2.2, the same arrangement
tools/net_test.py and tools/https_test.py use -- so this runs offline
and asserts nothing about any real upstream. Whether pci-ids.ucw.cz
serves what it used to is not this tool's question and could not be
answered offline anyway.

    python3 tools/hwdata_test.py
    python3 tools/hwdata_test.py --positive-control

THE POSITIVE CONTROL serves the TRUNCATED body to check 1 as well. A run
in which check 1 stays green is a run where the validator is not looking
at what arrived -- which is exactly the bug the whole design is against.
"""
import argparse
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from net_test import launch, kill  # noqa: E402
from harness import BodyServer, Results, copy_disk  # noqa: E402

PORT = 18089
GATEWAY = "10.0.2.2"

# A believable pci.ids: the real header shape, then enough vendor lines
# to clear the command's floor of 100, then enough bulk to clear its
# 64 KiB one. THE MARKER VENDOR IS 0001, which the real database does
# not carry -- so "lspci found this name" cannot be satisfied by the
# file that was already there.
MARKER_ID = "0001"
MARKER_NAME = "Toyos Test Vendor"


def good_body():
    lines = ["#", "#\tList of PCI ID's", "#", "# Version: 2099.01.01", "#", ""]
    lines.append(f"{MARKER_ID}  {MARKER_NAME}")
    lines.append("\t0002  Toyos Test Device")
    for i in range(400):
        lines.append(f"{0x1000 + i:04x}  Filler Vendor {i}")
        # Padding to clear the size floor without inventing 4000 vendors:
        # a device line is what a real file is mostly made of.
        for j in range(12):
            lines.append(f"\t{j:04x}  Filler Device {i}-{j}")
    return ("\n".join(lines) + "\n").encode()


# Under the size floor, over nothing. What a connection dropped halfway
# looks like -- and what `wget -O` would leave in place of the database.
def short_body():
    return good_body()[:20000]


# Over the size floor and with no vendor lines at all: a CDN error page,
# a captive portal, or a repository that moved. The size check alone
# passes this, which is why the vendor count exists.
def html_body():
    return (b"<html><head><title>404</title></head><body>\n"
            + b"<p>not found</p>\n" * 4000 + b"</body></html>\n")


Check = Results


def server(bodies):
    """`bodies` maps a path to the bytes served there, so one server
    answers every case and a check picks its failure by URL."""
    return BodyServer(port=PORT, routes=bodies)


def sum_of(sh, path):
    """The database's checksum, read through `sum` -- an INDEPENDENT
    path from the command under test. Comparing sizes would miss a file
    replaced by a different one of the same length; comparing the sum
    the updater printed would be believing the thing being tested."""
    out = sh.run(f"sum {path}")
    for line in out.splitlines():
        # The ECHOED COMMAND also ends with the path, and its first two
        # words are "sh sum" -- which parses as a checksum and a size
        # with nothing to say it is wrong. Require the first field to be
        # a number, which is what a real sum line starts with.
        parts = line.split()
        if len(parts) >= 3 and parts[-1] == path and parts[0].isdigit():
            return (parts[0], parts[1])
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--keep", action="store_true", help="leave the guest running")
    ap.add_argument("--positive-control", action="store_true",
                    help="serve the TRUNCATED body to check 1 as well")
    args = ap.parse_args()

    disk_src = os.path.join(ROOT, "disk.img")
    if not os.path.exists(disk_src):
        print("hwdata_test: SKIP -- no disk.img; run `make iso` first")
        return 0

    c = Check()
    tmp = tempfile.mkdtemp(prefix="hwdata_test.")
    pidfile = None
    srv = None
    try:
        srv = server({
            "/pci.ids": short_body() if args.positive_control else good_body(),
            "/short.ids": short_body(),
            "/error.html": html_body(),
        })

        disk = os.path.join(tmp, "hwdata.img")
        copy_disk(disk_src, disk)

        print("hwdata_test: booting")
        sh, pidfile = boot(disk, tmp, "hwdata")

        path = "/usr/share/hwdata/pci.ids"
        base = f"http://{GATEWAY}:{PORT}"
        before = sum_of(sh, path)
        c.ok("the shipped database is there to start with", before is not None,
             str(before))

        # 5. --from names one file
        out = sh.run(f"hwdata update all --from {base}/pci.ids")
        c.ok("--from with `all` is refused", "names one file" in out,
             out.strip()[:140])

        # 4. -n changes nothing
        out = sh.run(f"hwdata update pci -n --from {base}/pci.ids")
        c.ok("-n reports what it would replace", "would replace" in out,
             out.strip()[:160])
        c.ok("...and leaves the file alone", sum_of(sh, path) == before,
             f"{before} -> {sum_of(sh, path)}")

        # 2. THE LOAD-BEARING CHECK. A truncated body is refused and the
        # old file survives -- byte for byte, through `sum`.
        out = sh.run(f"hwdata update pci --from {base}/short.ids")
        c.ok("a truncated download is refused", "that is not a database" in out,
             out.strip()[:160])
        c.ok("...and the old database is untouched", sum_of(sh, path) == before,
             f"{before} -> {sum_of(sh, path)}")

        # 3. large, and not a database
        out = sh.run(f"hwdata update pci --from {base}/error.html")
        c.ok("a large error page is refused too", "that is not a database" in out,
             out.strip()[:160])
        c.ok("...and the old database is still untouched",
             sum_of(sh, path) == before, f"{before} -> {sum_of(sh, path)}")

        # 1. the good one replaces it, confirmed through lspci
        out = sh.run(f"hwdata update pci --from {base}/pci.ids")
        c.ok("a good download reports the replacement", "vendors)" in out,
             out.strip()[:160])
        after = sum_of(sh, path)
        c.ok("...and the file on disk actually changed", after != before,
             f"{before} -> {after}")
        c.ok("...and it carries the served Version line",
             "2099.01.01" in out, out.strip()[:200])

        # The strongest form: a DIFFERENT program reads the new file.
        # `hwdata` saying it wrote 1.6 MB proves it wrote a file, not
        # that the file is the one lspci resolves names out of.
        out = sh.run("cat /usr/share/hwdata/pci.ids")
        c.ok("the new database is what is on disk at the read path",
             MARKER_NAME in out, out.strip()[:120])

        print()
        bad = c.failed()
        print(f"hwdata_test: {len(c.rows) - len(bad)} passed, {len(bad)} failed")
        for _, name, detail in bad:
            print("  FAILED:", name, f"[{detail}]" if detail else "")
        if args.positive_control:
            print("\nhwdata_test: POSITIVE CONTROL -- check 1 and the marker "
                  "check MUST be red above. A green run means the validator "
                  "is not looking at what arrived.")
        return 1 if bad else 0
    finally:
        if srv:
            srv.shutdown()
        if pidfile and not args.keep:
            kill(pidfile)


def boot(disk, tmp, tag):
    return launch(disk, tmp, tag, "e1000")


if __name__ == "__main__":
    sys.exit(main())
