#!/usr/bin/env python3
"""tools/gen_kbs.py -- generates /etc/kbs/<layout> keyboard-layout data
files (kernel/lib/keyboard_layout.c's on-disk format) from Linux's own
XKB layout data, via `xkbcli compile-keymap` (libxkbcommon-tools --
`apt-get install libxkbcommon-tools` if missing; no X server needed, it
is a pure keymap compiler).

Why generate instead of hand-typing a table per layout: Linux already
has a maintained, correct mapping for every layout XKB knows about, and
re-deriving that by hand is exactly the busywork tools/ exists to avoid.

THE SHIPPED SET IS `LAYOUTS` BELOW, AND IT IS THE ONLY LIST. `--all`
generates every one of them (the Makefile's seed step calls that); the
display names Settings shows live in data/etc/settings.d/
system.keyboard_layout as `Choice.<name>=` lines, which `--choices`
prints from the same table (tools/check_docs.py fails when the two
disagree). A layout is in the set when everything its base and Shift
levels type is Latin-1 (toy-os's encoding, see docs/decisions/drivers.md)
AND no level carries a national letter Latin-1 lacks -- a precomposed
Latin letter such as Polish a-ogonek. `--check` re-measures both against
the host's XKB data and names the keys that break them; XKB's shared
AltGr extras (l-stroke, eng, dotless i...) are skipped without failing.

WHAT A FILE CARRIES:

- Four levels per key: base, Shift, AltGr (XKB level 3) and Shift+AltGr
  (level 4) -- `kc_<k>=`, `kc_<k>_shift=`, `kc_<k>_altgr=`,
  `kc_<k>_shift_altgr=`. Keyed by LINUX EVDEV KEYCODE: an XKB keycode is
  evdev + 8, a fixed convention, so `keycode - 8` needs no table. evdev
  is what virtio-input and USB report natively; only the PS/2 driver
  translates, as Linux's atkbd does.
- A character only when the font can draw it: ASCII 32-126 and Latin-1
  0xA0-0xFF. Anything else (a Polish AltGr+a's U+0105) is left out and
  listed in the file's trailing comment, and the key types nothing on
  that level.
- DEAD KEYS, with their compositions IN THE SAME FILE, so a layout is
  self-contained and /etc/kbs holds layouts and nothing else (its
  listing IS the Settings choice list):
      kc_26=dead:acute        the key is a dead acute on that level
      dead:acute=0xB4         what it types alone (dead + Space, or twice)
      dead:acute:e=0xE9       dead acute then e -> e-acute
  The pairs come from libX11's Compose table (COMPOSE_FILE), keeping
  only `<dead_X> <key>` whose result is Latin-1; without that file they
  fall back to Python's unicodedata, which agrees for Latin-1. A dead
  key with no Latin-1 spacing form and no Latin-1 composition (caron,
  ogonek, breve...) is unrepresentable and skipped like any other
  non-Latin-1 symbol. The spacing form is Windows' (dead acute + Space
  is the acute accent, not XKB's apostrophe).
- Values are a literal byte for printable ASCII, else `0x<hex>`.

Usage:
    python3 tools/gen_kbs.py de              # one layout to stdout
    python3 tools/gen_kbs.py de --write      # into seed/sync/etc/kbs/de
    python3 tools/gen_kbs.py --all --write   # every layout; prunes the rest
    python3 tools/gen_kbs.py --check         # does each still fit Latin-1?
    python3 tools/gen_kbs.py --choices       # the Choice.<name>= lines

Written under seed/sync/ deliberately: these files are pure build output,
like an ELF under seed/sync/bin/, so every `make iso` makes disk.img's
copy match the generator's.
"""

import os
import re
import subprocess
import sys
import unicodedata

# The shipped layouts: XKB name -> the display name Settings shows. ONE
# place; keep data/etc/settings.d/system.keyboard_layout in step
# (`--choices` prints its lines). NOT here, measured by --check, until
# the UTF-8 migration: `ee` (s/z-caron, its base TLDE is a dead caron),
# `lv`, `pl` and `ro`, whose national letters are all outside Latin-1.
LAYOUTS = {
    "al": "Albanian",
    "at": "German (Austria)",
    "be": "Belgian",
    "br": "Portuguese (Brazil)",
    "ca": "French (Canada)",
    "ch": "German (Switzerland)",
    "de": "German",
    "dk": "Danish",
    "es": "Spanish",
    "fi": "Finnish",
    "fo": "Faroese",
    "fr": "French",
    "gb": "English (UK)",
    "is": "Icelandic",
    "it": "Italian",
    "latam": "Spanish (Latin America)",
    "nl": "Dutch",
    "no": "Norwegian",
    "pt": "Portuguese",
    "se": "Swedish",
    "us": "English (US)",
}

COMPOSE_FILE = "/usr/share/X11/locale/en_US.UTF-8/Compose"

# X11 keysym names whose value is a Latin-1 codepoint (keysymdef.h: a
# keysym in 0x20-0x7E or 0xA0-0xFF IS that codepoint), embedded so the
# generator needs no X11 headers. A single letter or digit names itself
# and is not listed.
LATIN1_KEYSYMS = {
    "space": 0x20, "exclam": 0x21, "quotedbl": 0x22, "numbersign": 0x23,
    "dollar": 0x24, "percent": 0x25, "ampersand": 0x26, "apostrophe": 0x27,
    "quoteright": 0x27, "parenleft": 0x28, "parenright": 0x29,
    "asterisk": 0x2A, "plus": 0x2B, "comma": 0x2C, "minus": 0x2D,
    "period": 0x2E, "slash": 0x2F, "colon": 0x3A, "semicolon": 0x3B,
    "less": 0x3C, "equal": 0x3D, "greater": 0x3E, "question": 0x3F,
    "at": 0x40, "bracketleft": 0x5B, "backslash": 0x5C, "bracketright": 0x5D,
    "asciicircum": 0x5E, "underscore": 0x5F, "grave": 0x60, "quoteleft": 0x60,
    "braceleft": 0x7B, "bar": 0x7C, "braceright": 0x7D, "asciitilde": 0x7E,
    "nobreakspace": 0xA0, "exclamdown": 0xA1, "cent": 0xA2, "sterling": 0xA3,
    "currency": 0xA4, "yen": 0xA5, "brokenbar": 0xA6, "section": 0xA7,
    "diaeresis": 0xA8, "copyright": 0xA9, "ordfeminine": 0xAA,
    "guillemetleft": 0xAB, "guillemotleft": 0xAB, "notsign": 0xAC,
    "hyphen": 0xAD, "registered": 0xAE, "macron": 0xAF, "degree": 0xB0,
    "plusminus": 0xB1, "twosuperior": 0xB2, "threesuperior": 0xB3,
    "acute": 0xB4, "mu": 0xB5, "paragraph": 0xB6, "periodcentered": 0xB7,
    "cedilla": 0xB8, "onesuperior": 0xB9, "ordmasculine": 0xBA,
    "masculine": 0xBA, "guillemetright": 0xBB, "guillemotright": 0xBB,
    "onequarter": 0xBC, "onehalf": 0xBD, "threequarters": 0xBE,
    "questiondown": 0xBF, "Agrave": 0xC0, "Aacute": 0xC1, "Acircumflex": 0xC2,
    "Atilde": 0xC3, "Adiaeresis": 0xC4, "Aring": 0xC5, "AE": 0xC6,
    "Ccedilla": 0xC7, "Egrave": 0xC8, "Eacute": 0xC9, "Ecircumflex": 0xCA,
    "Ediaeresis": 0xCB, "Igrave": 0xCC, "Iacute": 0xCD, "Icircumflex": 0xCE,
    "Idiaeresis": 0xCF, "ETH": 0xD0, "Eth": 0xD0, "Ntilde": 0xD1,
    "Ograve": 0xD2, "Oacute": 0xD3, "Ocircumflex": 0xD4, "Otilde": 0xD5,
    "Odiaeresis": 0xD6, "multiply": 0xD7, "Oslash": 0xD8, "Ooblique": 0xD8,
    "Ugrave": 0xD9, "Uacute": 0xDA, "Ucircumflex": 0xDB, "Udiaeresis": 0xDC,
    "Yacute": 0xDD, "THORN": 0xDE, "Thorn": 0xDE, "ssharp": 0xDF,
    "agrave": 0xE0, "aacute": 0xE1, "acircumflex": 0xE2, "atilde": 0xE3,
    "adiaeresis": 0xE4, "aring": 0xE5, "ae": 0xE6, "ccedilla": 0xE7,
    "egrave": 0xE8, "eacute": 0xE9, "ecircumflex": 0xEA, "ediaeresis": 0xEB,
    "igrave": 0xEC, "iacute": 0xED, "icircumflex": 0xEE, "idiaeresis": 0xEF,
    "eth": 0xF0, "ntilde": 0xF1, "ograve": 0xF2, "oacute": 0xF3,
    "ocircumflex": 0xF4, "otilde": 0xF5, "odiaeresis": 0xF6, "division": 0xF7,
    "oslash": 0xF8, "ooblique": 0xF8, "ugrave": 0xF9, "uacute": 0xFA,
    "ucircumflex": 0xFB, "udiaeresis": 0xFC, "yacute": 0xFD, "thorn": 0xFE,
    "ydiaeresis": 0xFF,
    # Control keys: the same on every layout, but a file that omits them
    # leaves Enter dead (an early version of this script did).
    "Escape": 0x1B, "BackSpace": 0x08, "Tab": 0x09, "Return": 0x0A,
    "ISO_Left_Tab": 0x09,   # Shift+Tab: Tab, with Shift in the key's mods
}

# The dead keys Latin-1 can express: name -> (spacing form, the
# combining mark unicodedata decomposes to). The spacing form is what
# Windows types for dead + Space; XKB's Compose agrees for `dead dead`.
DEAD_KEYS = {
    "grave": (0x60, "̀"),
    "acute": (0xB4, "́"),
    "circumflex": (0x5E, "̂"),
    "tilde": (0x7E, "̃"),
    "macron": (0xAF, "̄"),
    "diaeresis": (0xA8, "̈"),
    "abovering": (0xB0, "̊"),
    "cedilla": (0xB8, "̧"),
}

# The physical alphanumeric block, plus AB11 (the Brazilian ABNT2 key
# beside right Shift). Order is for readable output only.
KEY_ORDER = (
    ["ESC"] +
    ["TLDE"] + [f"AE{i:02d}" for i in range(1, 13)] + ["BKSP"] +
    ["TAB"] + [f"AD{i:02d}" for i in range(1, 13)] +
    [f"AC{i:02d}" for i in range(1, 12)] + ["RTRN"] +
    ["BKSL"] +
    [f"AB{i:02d}" for i in range(1, 12)] +
    ["LSGT", "SPCE"]
)

LEVEL_SUFFIX = ("", "_shift", "_altgr", "_shift_altgr")


def keysym_cp(name):
    """A keysym's Latin-1 codepoint, or None."""
    if len(name) == 1 and name.isascii() and name.isalnum():
        return ord(name)
    if name in LATIN1_KEYSYMS:
        return LATIN1_KEYSYMS[name]
    m = re.fullmatch(r"U([0-9A-Fa-f]{4,6})", name)          # U00E9
    if m:
        cp = int(m.group(1), 16)
    else:
        m = re.fullmatch(r"0x0*1([0-9A-Fa-f]{6})", name)    # 0x010000e9
        if not m:
            return None
        cp = int(m.group(1), 16)
    return cp if (32 <= cp <= 126 or 0xA0 <= cp <= 0xFF) else None


KEYSYMDEF = "/usr/include/X11/keysymdef.h"
_KEYSYM_UNICODE = None


def keysym_unicode(name):
    """Any keysym's Unicode codepoint, or None: `Uxxxx`, a raw Unicode
    keysym, or a named one by keysymdef.h's U+ comments (read lazily;
    without the header only the first two are known)."""
    global _KEYSYM_UNICODE
    m = re.fullmatch(r"U([0-9A-Fa-f]{4,6})", name) or \
        re.fullmatch(r"0x0*1([0-9A-Fa-f]{6})", name)
    if m:
        return int(m.group(1), 16)
    if _KEYSYM_UNICODE is None:
        _KEYSYM_UNICODE = {}
        try:
            with open(KEYSYMDEF, encoding="latin-1") as f:
                for line in f:
                    k = re.match(r"#define XK_(\w+)\s+0x[0-9a-fA-F]+\s*/\*[ (]*U\+([0-9A-Fa-f]+)", line)
                    if k:
                        _KEYSYM_UNICODE[k.group(1)] = int(k.group(2), 16)
        except OSError:
            print(f"gen_kbs: {KEYSYMDEF} not found -- named non-Latin-1 "
                  "keysyms cannot be checked for national letters", file=sys.stderr)
    return _KEYSYM_UNICODE.get(name)


def is_national_letter(cp):
    """A precomposed Latin letter -- a base letter plus marks (a-ogonek,
    s-caron, t-comma). That is what a language's own alphabet adds; XKB's
    shared AltGr extras (l-stroke, d-stroke, eng, dotless i, long s) do
    not decompose and are not counted."""
    ch = chr(cp)
    d = unicodedata.normalize("NFD", ch)
    return (len(d) > 1 and unicodedata.category(d[0]).startswith("L")
            and "LATIN" in unicodedata.name(ch, ""))


def fmt(cp):
    """A value as the file writes it: literal when that cannot be
    misread (ASCII alphanumerics and punctuation other than the
    separators), else 0x<hex>."""
    if 33 <= cp <= 126 and chr(cp) not in "=:#":
        return chr(cp)
    if cp == 32:
        return " "
    return f"0x{cp:02X}"


def compile_keymap(layout):
    out = subprocess.run(
        ["xkbcli", "compile-keymap", "--layout", layout],
        capture_output=True, text=True, check=True,
    )
    return out.stdout


def parse_keycodes(keymap_text):
    """name -> XKB keycode, e.g. {'AE01': 10, ...}."""
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r"<(\w+)>\s*=\s*(\d+);", keymap_text)}


def parse_key_symbols(keymap_text):
    """name -> [level1, level2, ...] keysyms. A key block is either
    `{ [ a, A ] }` or `{ type= "...", symbols[1]= [ ... ] }` -- the
    second shape (German AE11's ss/?/backslash) is easy to miss."""
    syms = {}
    sym = keymap_text[keymap_text.index("xkb_symbols"):]
    for m in re.finditer(r"key\s*<(\w+)>\s*\{(.*?)\};", sym, re.S):
        body = m.group(2)
        g = re.search(r"symbols\[1\]\s*=\s*\[([^\]]*)\]", body) or \
            re.match(r"\s*\[([^\]]*)\]", body)
        if g:
            syms[m.group(1)] = [s.strip() for s in g.group(1).split(",")]
    return syms


def compose_pairs():
    """dead name -> {base cp: result cp}, Latin-1 both sides."""
    pairs = {name: {} for name in DEAD_KEYS}
    try:
        with open(COMPOSE_FILE, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        text = None
    if text is not None:
        for m in re.finditer(r'^<dead_(\w+)>\s*<(\w+)>\s*:\s*"([^"\\]|\\.)"',
                             text, re.M):
            dead, base, res = m.group(1), m.group(2), m.group(3)
            if dead not in pairs or base == "space" or base.startswith("dead_"):
                continue
            bcp = keysym_cp(base)
            rcp = ord(res[-1]) if len(res) == 1 else None
            if bcp is None or rcp is None or not 0xA0 <= rcp <= 0xFF:
                continue
            pairs[dead][bcp] = rcp
        return pairs
    print(f"gen_kbs: {COMPOSE_FILE} not found -- composing from unicodedata",
          file=sys.stderr)
    for cp in range(0xC0, 0x100):
        d = unicodedata.normalize("NFD", chr(cp))
        if len(d) != 2:
            continue
        for name, (_sp, mark) in DEAD_KEYS.items():
            if d[1] == mark:
                pairs[name][ord(d[0])] = cp
    return pairs


def generate(layout):
    """(file text, problems) -- a problem is a base/Shift symbol on the
    alphanumeric block that Latin-1 cannot express, or a national letter
    (is_national_letter) on any level."""
    text = compile_keymap(layout)
    keycodes = parse_keycodes(text)
    key_syms = parse_key_symbols(text)
    pairs = compose_pairs()

    lines = [
        f"# toy-os keyboard layout: {layout} ({LAYOUTS.get(layout, '?')})",
        f"# GENERATED by tools/gen_kbs.py from Linux's XKB '{layout}' layout -- do",
        "# not hand-edit. Format and limits: kernel/lib/keyboard_layout.c and",
        "# the generator's own top comment. Keys are LINUX EVDEV KEYCODES.",
        "",
    ]
    skipped, problems, deads = [], [], []
    for key in KEY_ORDER:
        if key not in keycodes or key not in key_syms:
            continue
        keycode = keycodes[key] - 8
        if keycode < 1 or keycode > 255:      # KB_KEYCODE_MAX
            continue
        levels = key_syms[key]
        for lvl, suffix in enumerate(LEVEL_SUFFIX):
            if lvl >= len(levels):
                continue
            sym = levels[lvl]
            if sym in ("NoSymbol", "VoidSymbol"):
                continue
            if sym.startswith("dead_") and sym[5:] in DEAD_KEYS:
                lines.append(f"kc_{keycode}{suffix}=dead:{sym[5:]}")
                if sym[5:] not in deads:
                    deads.append(sym[5:])
                continue
            cp = keysym_cp(sym)
            if cp is None:
                skipped.append((key, suffix or "_base", sym))
                if lvl < 2 and key not in ("ESC", "BKSP", "TAB", "RTRN"):
                    problems.append(f"{key}{suffix or ''}: {sym}")
                else:
                    u = keysym_unicode(sym)
                    if u is not None and is_national_letter(u):
                        problems.append(f"{key}{suffix}: {sym} (a letter Latin-1 lacks)")
                continue
            lines.append(f"kc_{keycode}{suffix}={fmt(cp)}")

    if deads:
        lines.append("")
        lines.append("# Dead keys: what each types alone, then its compositions.")
    for name in deads:
        lines.append(f"dead:{name}={fmt(DEAD_KEYS[name][0])}")
        for base, res in sorted(pairs[name].items()):
            lines.append(f"dead:{name}:{fmt(base)}={fmt(res)}")

    if skipped:
        lines.append("")
        lines.append("# Skipped -- not Latin-1, so this font cannot draw it:")
        for key, lvl, sym in skipped:
            lines.append(f"#   {key}{lvl}: {sym}")

    return "\n".join(lines) + "\n", problems


def write(layout, content):
    out_dir = os.path.join("seed", "sync", "etc", "kbs")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, layout)
    with open(out_path, "w", encoding="ascii") as f:
        f.write(content)
    return out_path


def prune():
    """Removes staged layouts no longer in LAYOUTS: the directory's
    listing IS the Settings choice list. (An existing disk.img keeps its
    copy -- the seed sync adds and replaces, never deletes.)"""
    out_dir = os.path.join("seed", "sync", "etc", "kbs")
    for name in os.listdir(out_dir) if os.path.isdir(out_dir) else ():
        if name not in LAYOUTS:
            os.remove(os.path.join(out_dir, name))
            print(f"gen_kbs: removed retired layout {name}", file=sys.stderr)


def main():
    args = sys.argv[1:]
    if "--choices" in args:
        for name in sorted(LAYOUTS, key=lambda n: LAYOUTS[n]):
            print(f"Choice.{name}={LAYOUTS[name]}")
        return 0
    if "--all" in args or "--check" in args:
        bad = failed = 0
        for layout in sorted(LAYOUTS):
            # ONE LAYOUT FAILING IS LOUD, and the rest are still written:
            # Settings lists /etc/kbs, so a silently missing file would be
            # a choice that falls back to US when picked.
            try:
                content, problems = generate(layout)
            except (subprocess.CalledProcessError, OSError) as e:
                failed += 1
                err = getattr(e, "stderr", "") or str(e)
                print(f"gen_kbs: FAILED to compile '{layout}': {err.strip()}",
                      file=sys.stderr)
                continue
            if problems:
                bad += 1
                print(f"gen_kbs: {layout} cannot be typed in Latin-1: "
                      + ", ".join(problems), file=sys.stderr)
            if "--write" in args:
                write(layout, content)
        if "--write" in args:
            prune()
            print(f"gen_kbs: wrote {len(LAYOUTS) - failed} layouts to "
                  "seed/sync/etc/kbs", file=sys.stderr)
        if failed:
            return 1
        return 1 if (bad and "--check" in args) else 0
    names = [a for a in args if not a.startswith("--")]
    if len(names) != 1:
        print(f"usage: {sys.argv[0]} <xkb-layout> [--write] | --all [--write] "
              "| --check | --choices", file=sys.stderr)
        return 1
    content, problems = generate(names[0])
    for p in problems:
        print(f"gen_kbs: {names[0]} cannot be typed in Latin-1: {p}", file=sys.stderr)
    if "--write" in args:
        print(f"gen_kbs: wrote {write(names[0], content)}", file=sys.stderr)
    else:
        sys.stdout.write(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())
