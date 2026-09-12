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

IT ALSO CHECKS A SETTING DESCRIPTOR'S TEXT against the ABI's own caps.
`Description=` and `Label=` cross the setting ABI in fixed-size fields
(`SETTING_ABI_DESC_MAX`, `SETTING_ABI_LABEL_MAX`), and a longer one is
`k_strlcpy`'d -- TRUNCATED MID-WORD, silently, with the file itself
still small enough to pass every other check here. That shipped: a
group description ended "...which is a s" on the page it introduced,
and only a person looking at the window could tell.

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
SETTING_ABI = os.path.join(REPO, "kernel/include/abi/setting_abi.h")
# The SCREENSAVER DESCRIPTORS have their own caps, in the library that
# parses them rather than in the settings ABI -- they are not settings.
USAVER_H = os.path.join(REPO, "userland/lib/usaver.h")
DIRS = ("data/etc", "data/usr/share/services")

# The descriptor keys that cross the ABI, and the constant bounding each.
# Read from the header rather than repeated here, same rule as the buffer
# limit above: a limit copied into a checker drifts from the code.
TEXT_KEYS = {
    "Description": "SETTING_ABI_DESC_MAX",
    "Label": "SETTING_ABI_LABEL_MAX",
}


def abi_caps():
    """The ABI's text caps, minus one for the NUL k_strlcpy writes."""
    src = open(SETTING_ABI).read()
    caps = {}
    for key, name in TEXT_KEYS.items():
        m = re.search(r"#define\s+" + name + r"\s+(\d+)", src)
        if m:
            caps[key] = int(m.group(1)) - 1
    return caps


def saver_caps():
    """`usaver.h`'s own caps, minus one for the NUL strlcpy writes.

    Read from the header for the same reason every other limit here is:
    a number copied into a checker drifts from the code that enforces it.
    """
    if not os.path.isfile(USAVER_H):
        return {}
    src = open(USAVER_H).read()
    out = {}
    for key, name in (("Label", "USAVER_LABEL_MAX"),
                      ("Desc", "USAVER_DESC_MAX"),
                      ("Unit", "USAVER_UNIT_MAX")):
        m = re.search(r"#define\s+" + name + r"\s+(\d+)", src)
        if m:
            out[key] = int(m.group(1)) - 1
    return out


def saver_text_problems():
    """Over-long per-option text in a screensaver descriptor.

    Same trap as the settings one below and a different set of fields: a
    `Desc.stars=` longer than the struct's array is `strlcpy`'d, so it
    reaches System Settings truncated mid-word with the file itself far
    under every size limit here.
    """
    caps = saver_caps()
    if not caps:
        return []
    out = []
    for rel in tracked("data/wm/savers"):
        path = os.path.join(REPO, rel)
        if not os.path.isfile(path) or rel.endswith(".md"):
            continue
        for line in open(path, encoding="utf-8", errors="replace"):
            line = line.rstrip("\n")
            if "=" not in line or line.lstrip().startswith("#"):
                continue
            key, _, value = line.partition("=")
            # `Label.colour` -> `Label`; the per-option keys are the only
            # ones with a suffix, and `Options=` has none.
            cap = caps.get(key.strip().split(".", 1)[0])
            if cap is not None and len(value.encode("utf-8")) > cap:
                out.append((rel, key.strip(), len(value.encode("utf-8")), cap, value))
    return out


def descriptor_text_problems(caps):
    """Over-long Description=/Label= lines in every settings descriptor."""
    out = []
    for rel in tracked("data/etc/settings.d"):
        path = os.path.join(REPO, rel)
        if not os.path.isfile(path) or rel.endswith(".md"):
            continue
        for line in open(path, encoding="utf-8", errors="replace"):
            line = line.rstrip("\n")
            if "=" not in line or line.lstrip().startswith("#"):
                continue
            key, _, value = line.partition("=")
            cap = caps.get(key.strip())
            if cap is not None and len(value.encode("utf-8")) > cap:
                out.append((rel, key.strip(), len(value.encode("utf-8")), cap, value))
    return out


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

    caps = abi_caps()
    long_text = descriptor_text_problems(caps) if caps else []
    long_text += saver_text_problems()
    for rel, key, n, lim, value in long_text:
        print(f"  {rel}: {key} is {n} bytes, over the {lim}-byte ABI field "
              f"-- it would be TRUNCATED MID-WORD in System Settings, at "
              f"...{value[:lim][-14:]!r}")

    for rel, n in bad:
        print(f"  {rel} is {n} bytes, over the {cap}-byte config limit "
              f"-- its last keys would be IGNORED, and the machine would "
              f"report the missing key rather than the size")
    if bad or long_text:
        if bad:
            print(f"check_config_size: FAIL -- {len(bad)} of {seen} file(s) "
                  f"too large. Move the prose to docs/commands/; the "
                  f"descriptor is configuration.")
        if long_text:
            print(f"check_config_size: FAIL -- {len(long_text)} descriptor "
                  f"field(s) over the ABI cap. A System Settings description "
                  f"is one short line, not a paragraph.")
        return 1
    print(f"check_config_size: ok -- {seen} config file(s), all within "
          f"{cap} bytes; descriptor text within the ABI and usaver caps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
