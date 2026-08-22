#!/usr/bin/env python3
"""tools/gen_kbs.py -- generates /etc/kbs/<layout> keyboard-layout data
files (kernel/lib/keyboard_layout.c's on-disk format) from the Linux
side's own XKB layout data, via `xkbcli compile-keymap` (part of
libxkbcommon-tools -- `apt-get install libxkbcommon-tools` if missing;
no X server needed, it's a pure keymap compiler).

Why generate instead of hand-typing 128 scancode/character pairs per
layout: Linux already has a maintained, correct mapping for every
layout XKB knows about (`ls /usr/share/X11/xkb/symbols/` for the full
list) -- re-deriving that by hand is exactly the kind of error-prone
busywork this project's tools/ directory exists to avoid (see
CLAUDE.md's tools/ section). Adding a third layout later (say German)
is `python3 tools/gen_kbs.py de` plus a re-seed, not an afternoon with
a scancode chart.

WHAT THIS DOES NOT DO -- read before trusting a generated file blindly:

- Reads shift levels 1-3 (base, Shift, AltGr) out of each key's symbol
  list -- level 4 (Shift+AltGr) is still ignored: keyboard.c's AltGr
  handling only tracks "is AltGr down," not "is AltGr down at the same
  time as Shift" as a distinct fourth state, so a level-4-only symbol
  (rare in practice -- most real layouts, including Finnish/Swedish,
  don't put anything meaningful there) would be unreachable even if
  translated. A real Finnish/Swedish keyboard's @ # $ { } [ ] (all
  level-3/AltGr on those layouts) ARE reachable now via `sc_XX_altgr`
  entries below. Known partial limitation, not a bug in this script.
- Only emits a character for keysyms this kernel's font can actually
  render: ASCII 32-126, plus the six Nordic Latin-1 codepoints
  kernel/drivers/font_ttf.c bakes (Ä Ö Å ä ö å -- see genttf.py's
  EXTRA_CHARS). A keysym outside that set (e.g. Swedish TLDE's
  section-sign/paragraph-mark) is silently skipped -- that scancode's
  slot in the output file is simply absent, which keyboard_layout.c
  treats the same as "no char at this scancode" (produces nothing when
  pressed), matching how an unmapped slot already behaves today.
- Dead keys (dead_acute, dead_grave, ...) aren't composed (no dead-key
  state machine in this driver) -- mapped instead to the plain visible
  character closest to what they'd produce undead (dead_acute -> ',
  dead_grave -> `), same simplification XKB's own "nodeadkeys" layout
  variants make for the same reason.
- XKB key names -> Linux EVDEV KEYCODES: the keymap's `xkb_keycodes`
  section gives each key an XKB keycode, and XKB keycodes are evdev
  keycodes + 8 (a fixed, decades-old convention). So `keycode - 8`
  below IS the evdev keycode, with no lookup table anywhere.

  **THESE FILES USED TO BE KEYED ON AT SET-1 SCANCODES, and the switch
  is the point rather than a rename.** evdev is what every non-PS/2
  keyboard reports natively (virtio-input, and a USB keyboard when there
  is one), so keying the layout on scancodes forced every such driver to
  translate UPWARD into a legacy encoding -- a hand-kept table pointing
  the wrong way, which duly grew a hole: KEY_102ND, the ISO key carrying
  `|` on every Nordic layout, was missing, so a pipeline could be typed
  on PS/2 and not on virtio. Keyed on evdev, there is no table to have a
  hole in: the one translation left is set-1 -> keycode inside the PS/2
  driver, which is exactly where Linux keeps it (`atkbd`).

  It happens that evdev and set 1 AGREE for the whole primary block
  (KEY_1 = 2 = 0x02, and so on up to KEY_F12 = 88 = 0x58), which is why
  the values in these files did not change when the keying did -- and
  also why the old naming looked right for years.

Usage:
    python3 tools/gen_kbs.py us > seed/sync/etc/kbs/us
    python3 tools/gen_kbs.py se > seed/sync/etc/kbs/se
    python3 tools/gen_kbs.py se --write   # writes seed/sync/etc/kbs/se directly

Written under seed/sync/ (not seed/once/) deliberately: these files
are pure build output of this generator, the same as an ELF binary
under seed/sync/bin/ -- every `make iso` should make disk.img's copy
match the repo's exactly, the same content-hash-synced policy the
existing `bin` binaries already get (see tools/tfs2_writer.py's
`sync` docstring / CLAUDE.md's tools/ section for the once/ vs sync/
split). seed/once/ is for content a session/user might have
legitimately changed on the emulated disk since -- not the case here.
"""

import re
import subprocess
import sys

# XKB keysym name -> the character it produces, restricted to what
# this kernel can actually print (ASCII 32-126 + the six Nordic Latin-1
# codepoints -- see the module docstring). Letters/digits aren't listed
# here: a single-char lowercase-letter or digit keysym name IS the
# character (XKB names them literally, e.g. keysym "q" means 'q').
KEYSYM_TABLE = {
    "exclam": "!", "at": "@", "numbersign": "#", "dollar": "$",
    "percent": "%", "asciicircum": "^", "ampersand": "&",
    "asterisk": "*", "parenleft": "(", "parenright": ")",
    "minus": "-", "underscore": "_", "equal": "=", "plus": "+",
    "bracketleft": "[", "braceleft": "{", "bracketright": "]",
    "braceright": "}", "backslash": "\\", "bar": "|",
    "semicolon": ";", "colon": ":", "apostrophe": "'",
    "quotedbl": '"', "comma": ",", "less": "<", "period": ".",
    "greater": ">", "slash": "/", "question": "?", "grave": "`",
    "asciitilde": "~", "space": " ",
    # Nordic letters -- Latin-1 codepoints, matching
    # kernel/include/api/keyboard.h's CHAR_A_RING/CHAR_A_DIAERESIS/etc.
    "aring": "å", "Aring": "Å",
    "adiaeresis": "ä", "Adiaeresis": "Ä",
    "odiaeresis": "ö", "Odiaeresis": "Ö",
    # Dead keys: not composed (see module docstring) -- substituted
    # with the plain undead glyph, same simplification XKB's own
    # "nodeadkeys" layout variants make.
    "dead_acute": "'", "dead_grave": "`",
    # Control keys -- same identical to every layout (Escape/Backspace/
    # Tab/Enter don't move or change meaning between US and SE/FI
    # physical keyboards), but still have to go through this table:
    # keyboard.c's old compiled-in scancode_ascii[] included these
    # (scancode 0x01/0x0E/0x0F/0x1C), and a layout file that omits them
    # means that key produces nothing at all -- not a fallback to some
    # other behavior, just silently dead. Caught live: an early version
    # of this script left these out of KEY_ORDER entirely, and Enter
    # stopped working the moment the shell switched from keyboard.c's
    # compiled-in tables to loading a generated file.
    "Escape": chr(27), "BackSpace": "\b", "Tab": "\t", "Return": "\n",
}

# The physical alphanumeric-block key names this matters for -- XKB
# also defines names for modifiers, function keys, the numpad, etc,
# none of which this generator (or keyboard.c's ASCII tables) cares
# about. Order here is just for readable output; doesn't affect
# correctness.
KEY_ORDER = (
    ["ESC"] +
    ["TLDE"] + [f"AE{i:02d}" for i in range(1, 13)] + ["BKSP"] +
    ["TAB"] + [f"AD{i:02d}" for i in range(1, 13)] +
    [f"AC{i:02d}" for i in range(1, 12)] + ["RTRN"] +
    ["BKSL"] +
    [f"AB{i:02d}" for i in range(1, 11)] +
    ["LSGT", "SPCE"]
)


def keysym_to_char(name):
    if len(name) == 1 and (name.isalpha() or name.isdigit()):
        return name
    return KEYSYM_TABLE.get(name)


def compile_keymap(layout):
    out = subprocess.run(
        ["xkbcli", "compile-keymap", "--layout", layout],
        capture_output=True, text=True, check=True,
    )
    return out.stdout


def parse_keycodes(keymap_text):
    """name -> XKB keycode, e.g. {'AE01': 10, ...}."""
    codes = {}
    for m in re.finditer(r"<(\w+)>\s*=\s*(\d+);", keymap_text):
        codes[m.group(1)] = int(m.group(2))
    return codes


def parse_key_symbols(keymap_text):
    """name -> [level1_keysym, level2_keysym, ...] from the compiled
    xkb_symbols section's `key <NAME> { [ sym, sym, ... ] };` lines."""
    syms = {}
    for m in re.finditer(r"key\s*<(\w+)>\s*\{\s*\[([^\]]*)\]", keymap_text):
        name = m.group(1)
        levels = [s.strip() for s in m.group(2).split(",")]
        syms[name] = levels
    return syms


def generate(layout):
    text = compile_keymap(layout)
    keycodes = parse_keycodes(text)
    key_syms = parse_key_symbols(text)

    lines = [
        f"# toy-os keyboard layout: {layout}",
        f"# Generated by tools/gen_kbs.py from Linux's own XKB '{layout}' layout --",
        "# do not hand-edit without re-running the generator (see that script's",
        "# top comment for what it does and doesn't translate: AltGr level 3 only",
        "# (no Shift+AltGr level 4), no dead keys, only the 6 baked Nordic glyphs).",
        "# Format: name=value, one 'kc_<decimal evdev keycode>=<char>' /",
        "# 'kc_<keycode>_shift=<char>' / 'kc_<keycode>_altgr=<char>' pair per key;",
        "# <char> is a literal single character, or 0xNN for a codepoint above",
        "# ASCII (the Nordic letters). See kernel/lib/keyboard_layout.c.",
        "#",
        "# KEYCODES ARE LINUX EVDEV NUMBERS, not AT scancodes -- decimal, and",
        "# the same numbers virtio-input and a USB keyboard report natively.",
        "",
    ]

    missing = []
    for key in KEY_ORDER:
        if key not in keycodes or key not in key_syms:
            continue
        keycode = keycodes[key] - 8
        # KEYCODE_MAX in kernel/lib/keyboard_layout.c. Above it are media
        # and consumer keys with no character to map.
        if keycode < 1 or keycode > 255:
            continue
        levels = key_syms[key]
        for level_idx, suffix in ((0, ""), (1, "_shift"), (2, "_altgr")):
            if level_idx >= len(levels):
                continue
            ch = keysym_to_char(levels[level_idx])
            if ch is None:
                if levels[level_idx] not in ("NoSymbol", "VoidSymbol"):
                    missing.append((key, levels[level_idx]))
                continue
            cp = ord(ch)
            value = ch if 32 <= cp <= 126 else f"0x{cp:02X}"
            lines.append(f"kc_{keycode}{suffix}={value}")

    if missing:
        lines.append("")
        lines.append("# Skipped -- keysym has no glyph in this kernel's font "
                      "(see this script's top comment):")
        for key, sym in missing:
            lines.append(f"#   {key}: {sym}")

    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <xkb-layout> [--write]", file=sys.stderr)
        sys.exit(1)
    layout = sys.argv[1]
    content = generate(layout)
    if "--write" in sys.argv[2:]:
        import os
        out_dir = os.path.join("seed", "sync", "etc", "kbs")
        os.makedirs(out_dir, exist_ok=True)
        out_path = os.path.join(out_dir, layout)
        with open(out_path, "w") as f:
            f.write(content)
        print(f"gen_kbs: wrote {out_path}", file=sys.stderr)
    else:
        sys.stdout.write(content)


if __name__ == "__main__":
    main()
