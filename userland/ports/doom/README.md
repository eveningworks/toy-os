# doomgeneric -- vendored, not written here

**This directory is third-party source. Do not "fix" it to match this
project's conventions** -- its value is precisely that nobody working on
toy-os wrote it. An edit here is a divergence from upstream that
somebody has to carry forever; if something needs changing, the answer
is almost always to change the C library, the toolkit, or the backend in
`userland/doom/` instead.

**TWO UPSTREAMS LIVE HERE, and the second is not a different project.**
doomgeneric IS Chocolate Doom with its platform layer and sound removed
-- 24 files here carry Simon Howard's copyright, and `sound_module_t`,
`music_module_t` and the GENMIDI handling are all Chocolate Doom's
design. So the music files below were not adapted from somewhere else;
they are the rest of this port, taken from where it came from.

- **Upstream**: https://github.com/ozkl/doomgeneric
- **Commit**: `dcb7a8dbc7a1` (2026-04-12)
- **Files**: the 80 `.c` files of upstream's own `SRC_DOOM` list, minus
  its platform backend, plus every `.h` in that directory and
  `LICENSE` -- copied VERBATIM, byte for byte. No local patches.

- **Second upstream**: https://github.com/chocolate-doom/chocolate-doom
- **Tag**: `chocolate-doom-2.1.0` -- chosen by MEASUREMENT, not by
  guess: at that tag `memio.c` is byte-identical to doomgeneric's copy
  and `i_sound.h` differs only by the declarations doomgeneric appended
  to the end, so `sound_module_t` and `music_module_t` match exactly.
- **Files**: `i_oplmusic.c`, `midifile.c`, `midifile.h`, `mus2mid.c`
  (whose header doomgeneric already shipped without it), and
  `opl/{opl.c,opl.h,opl_internal.h,opl_queue.c,opl_queue.h,dbopl.c,dbopl.h}`
  -- again VERBATIM. Same GPL-2.0, same aggregation.
- **NOT taken**: `opl/`'s platform drivers (`opl_sdl.c`, `opl_linux.c`,
  `opl_win32.c`, `opl_obsd.c`, `ioperm_sys.c`) and `i_sdlsound.c`. Ours
  replace them, from `userland/doom/`.
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
dispatcher. Upstream leaves `FEATURE_SOUND` undefined, so its module
list is empty and it resolves to silence; toy-os defines the flag on the
COMPILER COMMAND LINE (see the Makefile) rather than editing
`doomfeatures.h`, and supplies `DG_sound_module` and `DG_music_module`
from `userland/doom/`.

## The three shims, and why none of them is a patch

Turning the flag on makes this code reach for things the SDL port had.
Each is answered from OUR side of the boundary, so not one byte here
changed:

- **`SDL_mixer.h`** -- `i_sound.c` includes it under `FEATURE_SOUND` and
  never uses a symbol from it. `userland/doom/compat/SDL_mixer.h` is
  empty, and says so.
- **`SDL.h`** -- `opl.c` and `midifile.c` want big-endian byte swaps and
  a mutex/condition pair. `userland/doom/compat/SDL.h` maps those onto
  `__builtin_bswap` and pthreads, so they genuinely work rather than
  being stubbed.
- **`opl_sdl_driver`** -- `opl.c`'s driver list names that symbol
  unconditionally, so `userland/doom/opl_toyos.c` exports it. A
  link-time substitution; the driver's own `name` field says `toyos`.

The alternative to all three was editing vendored files, which this
directory exists to avoid.

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
