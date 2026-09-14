# Colour schemes

One `<name>.scheme` per palette the GUI Terminal offers. The file name
without the extension is the name; `/etc/terminal.conf`'s `scheme=` key
holds the chosen one. See `userland/term/term.h` for the format and
`docs/filesystem-layout.md` for why they live here rather than beside
the binary.

**`Color0`..`Color15` are in ANSI order** -- black, red, green, yellow,
blue, magenta, cyan, white, then the eight bright forms -- which is the
order every published palette is written in, so one can be copied in
without being permuted by hand. The cells the emulator stores are VGA
indices; `term_scheme_load()` does the permutation.
