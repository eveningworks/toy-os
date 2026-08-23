# doomgeneric -- vendored, not written here

**This directory is third-party source. Do not "fix" it to match this
project's conventions** -- its value is precisely that nobody working on
toy-os wrote it. An edit here is a divergence from upstream that
somebody has to carry forever; if something needs changing, the answer
is almost always to change the C library, the toolkit, or the backend in
`userland/doom/` instead.

- **Upstream**: https://github.com/ozkl/doomgeneric
- **Commit**: `dcb7a8dbc7a1` (2026-04-12)
- **Files**: the 80 `.c` files of upstream's own `SRC_DOOM` list, minus
  its platform backend, plus every `.h` in that directory and
  `LICENSE` -- copied VERBATIM, byte for byte. No local patches.
- **Licence**: **GPL-2.0**, and this is the one directory in toy-os that
  is not MIT. See "Licensing" below -- it matters, and it is why this is
  linked into exactly one binary.

## Why it is here

doomgeneric is Doom with the platform layer reduced to five functions
(`DG_Init`, `DG_DrawFrame`, `DG_SleepMs`, `DG_GetTicksMs`,
`DG_GetKey`), which is what makes "port Doom" a backend rather than a
rewrite. It is the target `docs/roadmap-details.md` named for this work
years of sessions before it happened.

It is here for the same reason `../cjson/` is, one size up: **a real
program nobody working on this repo wrote, built against this OS.**
cJSON proved the C library; ~36,000 lines of Doom prove the whole ring-3
stack at once -- the allocator, stdio over a real file, the ELF loader,
the stack, floating point, the window protocol and the input path.

## What we DID NOT take

Upstream ships a backend per platform (`doomgeneric_sdl.c`,
`_xlib.c`, `_win.c`, and others) and sound/music modules for SDL and
Allegro. None are here: ours is `userland/doom/dg_toyos.c`, which is
ours and lives outside this directory precisely so that the boundary
between vendored and written-here is a directory boundary.

`i_sound.c` IS here and IS compiled -- it is the generic sound
dispatcher, and with no sound module registered it resolves to silence
on its own. That is upstream behaviour, not a patch.

## Licensing, stated plainly

toy-os is MIT (`/LICENSE`). doomgeneric is GPL-2.0, inherited from the
1997 Doom source release. Those coexist here as an **aggregation**: this
directory is a separate program that happens to live in the same
repository, and the Makefile links it into exactly ONE binary
(`EXTRA_OBJS_doom`) so that nothing else in the tree can accidentally
depend on GPL code. Nothing in `libuapp.a` or `libc.a` links against
it, and it links against them -- which is the direction that matters.

**The WAD is a separate question from the code**, and a separate licence
again: id's shareware `doom1.wad` is not GPL and is not in this
repository. See `docs/decisions.md`.
