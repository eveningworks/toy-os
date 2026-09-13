# dash -- vendored, not written here

**This directory is third-party source. Do not "fix" it to match this
project's conventions** -- its value is precisely that nobody working on
toy-os wrote it. An edit here is a divergence from upstream that
somebody has to carry forever; if something needs changing, the answer
is almost always to change the C library, or the platform glue in
`userland/backends/`, instead.

- **Upstream**: https://git.kernel.org/pub/scm/utils/dash/dash.git
- **Tag**: `v0.5.13.5`
- **Obtained as**: that repository's own
  `snapshot/dash-0.5.13.5.tar.gz`, sha256
  `53622e51df0fd7a2950552cfe0da0bee9bfc1510784e752b62eacc49b3776d33`
- **Files**: the WHOLE upstream tree, copied verbatim, byte for byte --
  including the autotools files, which nothing here runs. They are kept
  because `src/Makefile.am` is the authoritative statement of which
  sources build and which generator produces which file, and a port
  rule derived from a remembered list is the thing this project keeps
  deleting. No local patches.

**NOTHING BUILDS THIS YET.** There is no Makefile rule for it, which is
why vendoring it changes nothing: `userland/ports/` is not
auto-discovered, each port is named explicitly (compare
`DOOM_PORT_SRCS`). See `docs/roadmap.md`'s "A ported POSIX shell" for
what the C library still owes it.

## Licence, and the one file that is not BSD

dash is BSD-3-Clause: Berkeley, Kenneth Almquist, Christos Zoulas and
Herbert Xu, full text in `COPYING`.

**`src/mksignames.c` IS GPL-2-OR-LATER**, from GNU Bash, and `COPYING`
says so itself in a second section: *"This file is not directly linked
with dash. However, its output is."* It is a build-time generator that
emits `signames.c`, the signal-name table `kill -l` and `trap` print.

That matters here because "BSD-licensed, no GPL into an MIT tree" is
the stated reason this project chose dash over BusyBox `ash`
(`docs/roadmap-details.md`), and that premise is true of dash's shell
but not of its build. toy-os does not have to inherit the problem: our
signal set is our own (`kernel/include/abi/signal_abi.h`), so the table
has to be generated from OUR numbers regardless, and a replacement
generator is a few lines. Until one exists, this file is vendored and
disclosed rather than quietly dropped -- see `LICENSE`.

## Why dash

`tosh` is ours and was never going to grow into a POSIX shell. dash is
the Almquist shell BusyBox's `ash` was re-synced from, is what Debian
and Ubuntu ship as `/bin/sh`, and carries no Kconfig or applet
framework to vendor around.

It brings no line editing -- Debian builds it without libedit, so
interactively it has no arrow keys and no history. The port wires
`kernel/lib/klineedit.c` into its read-a-line seam instead, so the keys
do not change with the shell.
