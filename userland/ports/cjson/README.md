# cJSON -- vendored, not written here

**This directory is third-party source. Do not "fix" it to match this
project's conventions** -- its value is precisely that nobody working on
toy-os wrote it. An edit here is a divergence from upstream that
somebody has to carry forever; if something needs changing, the answer
is almost always to change the C library instead.

- **Upstream**: https://github.com/DaveGamble/cJSON
- **Commit**: `fb16e5cf3587` (2026-04-09)
- **Files**: `cJSON.c`, `cJSON.h`, `LICENSE` -- copied VERBATIM, byte for
  byte, from that commit. No local patches.
- **Licence**: MIT. `LICENSE` is the upstream file and must stay with
  the source; the copyright notice in `cJSON.c`'s header is part of it.

## Why it is here

`docs/libc-design.md`'s Stage 6: **build and run a program nobody
working on this repo wrote.** A test that only exercises code written to
pass it proves nothing about a C library, and every other test in
`userland/tests/` was written by someone who knew what the library
supported.

cJSON was picked because it leans on almost exactly what Stages 2-4
built -- `malloc`/`realloc`, `strtod`, `sprintf`, `sscanf`, `<math.h>`
and most of `<string.h>` -- and because parsing then re-serialising JSON
makes the proof a ROUND TRIP through foreign code rather than "it
printed something".

## What it found

It did not build on the first try, which is the point. The gaps were
real and are recorded in `docs/libc-design.md`; the fixes went into the
C library, not into this directory.

`userland/tests/cjson_test.c` is the harness, and it is ours.
