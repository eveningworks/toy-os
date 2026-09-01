#!/usr/bin/env python3
"""No shipped config file may exceed the parser's buffer.

WHY THIS EXISTS
---------------
`etc_config.c` reads a whole `name=value` document into a fixed buffer
(`ETC_CONFIG_BUF_MAX`). A file larger than that is READ SHORT: the keys
past the cut are simply not seen, and what the machine reports is the
consequence rather than the cause --

    init: /etc/services.d/dhcp is longer than the config parser's buffer
    init: dhcp has no Exec=, will not start it

The second line is the one a person reads, and it says "broken service"
when the truth is "long comment". That happened while writing the
comment on a service descriptor, and cost an hour partly because the
limit was then attributed to the WRONG constant -- there are two, and
the one that fires for a read is not the smaller one.

**Comments count.** The buffer holds the file, not the settings, so
prose in a descriptor consumes the same budget as keys. That is the
whole trap: the file that fails looks nothing like too much
configuration.

WHAT IT CHECKS
--------------
Every tracked file under `data/etc/` and `data/usr/share/services/`
against ETC_CONFIG_BUF_MAX, read from the header rather than repeated
here -- a limit copied into a checker is a limit that drifts from the
code it checks.

WHAT IT DOES NOT CHECK, said plainly: files written at RUNTIME.
`/etc/resolv.conf`, `/etc/desktop.conf` and the settings files grow as
the system uses them, and nothing here can see that. The rewrite path
refuses rather than truncating (etc_config.h), so those fail safely --
but they fail at runtime, and this cannot warn about them.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
HEADER = os.path.join(REPO, "kernel/include/api/etc_config.h")
DIRS = ("data/etc", "data/usr/share/services")


def limit():
    src = open(HEADER).read()
    m = re.search(r"#define\s+ETC_CONFIG_BUF_MAX\s+(\d+)", src)
    if not m:
        sys.exit("check_config_size: ETC_CONFIG_BUF_MAX not found in "
                 "kernel/include/api/etc_config.h")
    # The reader stores a NUL, so the usable size is one less.
    return int(m.group(1)) - 1


def tracked(d):
    full = os.path.join(REPO, d)
    if not os.path.isdir(full):
        return []
    r = subprocess.run(["git", "ls-files", d], cwd=REPO,
                       capture_output=True, text=True)
    return [p for p in r.stdout.split() if p]


def main():
    cap = limit()
    bad, seen = [], 0
    for d in DIRS:
        for rel in tracked(d):
            path = os.path.join(REPO, rel)
            if not os.path.isfile(path) or rel.endswith(".md"):
                continue
            seen += 1
            n = os.path.getsize(path)
            if n > cap:
                bad.append((rel, n))

    for rel, n in bad:
        print(f"  {rel} is {n} bytes, over the {cap}-byte config limit "
              f"-- its last keys would be IGNORED, and the machine would "
              f"report the missing key rather than the size")
    if bad:
        print(f"check_config_size: FAIL -- {len(bad)} of {seen} file(s) too "
              f"large. Move the prose to docs/commands/; the descriptor is "
              f"configuration.")
        return 1
    print(f"check_config_size: ok -- {seen} config file(s), all within "
          f"{cap} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
