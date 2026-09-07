# Mbed TLS -- vendored, not written here

**This directory is third-party source. Do not "fix" it to match this
project's conventions** -- its value is precisely that nobody working on
toy-os wrote it. An edit here is a divergence from upstream that
somebody has to carry forever; if something needs changing, the answer
is almost always a macro in `userland/backends/mbedtls/toyos_mbedtls_config.h`
or a flag in the Makefile.

- **Upstream**: https://github.com/Mbed-TLS/mbedtls
- **Branch**: `mbedtls-3.6` (the LTS branch)
- **Commit**: `091fd1b18806` (2026-09-03)
- **Licence**: Apache-2.0 OR GPL-2.0-or-later, at the user's option.
  **toy-os takes Apache-2.0**, which aggregates with an MIT tree without
  reaching any of it. `LICENSE` is the upstream file and must stay with
  the source.

## What was taken, and what was not

Copied VERBATIM, byte for byte, from that commit:

- `library/*.c` and `library/*.h`
- `include/mbedtls/*.h`, `include/psa/*.h`
- `LICENSE`

Deliberately NOT taken: `programs/`, `tests/`, `docs/`, `scripts/`,
`framework/`, `configs/`, `3rdparty/`, and the CMake/Makefile build
system. The last two need a word each.

**`3rdparty/`** holds Everest's X25519 and the `p256-m` driver, which
are alternative implementations selected by
`MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED` and
`MBEDTLS_PSA_P256M_DRIVER_ENABLED`. This configuration enables neither,
which was verified rather than assumed: the tree compiles with no
`-I` pointing at either directory.

**The build system** is replaced by rules in this repository's Makefile,
which is the same call `userland/ports/doom/` makes. Upstream's file
list is WILDCARDED there rather than copied, so a file added by a future
update needs no Makefile edit.

## Five files here are GENERATED, and are committed on purpose

Upstream does not ship them. Its build runs Python (over Jinja2
templates) and Perl scripts to produce them:

| File | Produced by |
|---|---|
| `library/psa_crypto_driver_wrappers.h` | `scripts/generate_driver_wrappers.py` |
| `library/psa_crypto_driver_wrappers_no_static.c` | the same script |
| `library/ssl_debug_helpers_generated.c` | `framework/scripts/generate_ssl_debug_helpers.py` |
| `library/error.c` | `scripts/generate_errors.pl` |
| `library/version_features.c` | `scripts/generate_features.pl` |

They are committed so that **no toy-os build ever needs Python, Jinja2,
jsonschema or Perl**. This project's gate deliberately refuses to start
requiring a tool a fresh checkout may not have -- the same rule that
keeps Docker out of `preflight.sh` -- and a build that fails on a
missing Python package is exactly that failure. BearSSL makes the same
call with its T0-generated state machines, which is where the idea came
from.

**To regenerate them after an upstream update**, in a checkout of
upstream at the new commit (the framework submodule is needed, and is
not vendored here):

    git submodule update --init framework
    python3 -m venv /tmp/mbedtls-gen
    /tmp/mbedtls-gen/bin/pip install jinja2 jsonschema
    /tmp/mbedtls-gen/bin/python scripts/generate_driver_wrappers.py
    cd library
    /tmp/mbedtls-gen/bin/python \
        ../framework/scripts/generate_ssl_debug_helpers.py --mbedtls-root .. .
    perl ../scripts/generate_errors.pl
    perl ../scripts/generate_features.pl

then copy the five files in with the rest.

## What it takes to build freestanding

Two flag groups in the Makefile, both non-obvious enough to be worth
repeating here because a future update will meet them again:

**`-nostdinc`**, plus `-isystem` pointing at GCC's own include
directory. Nothing else in this build passes `-nostdinc`, so a header
tolibc lacks resolves silently to `/usr/include`'s -- and
`library/x509_crt.c` asks `__has_include(<sys/socket.h>)`, which cares
only whether the file is on the path and not what the target is. It
then took the host's, which conflicts with our own `<sys/types.h>`.
This is also why tolibc needed a `<limits.h>`: GCC's ends in
`#include_next <limits.h>`, so a `-nostdinc` compile fails inside GCC's
own header when the C library has none.

**`-Uunix -U__unix -U__unix__ -U__linux__ -U__gnu_linux__`.** GCC
predefines all five for every toy-os compile, because the host triple is
Linux. `library/common.h` reads them and defines
`MBEDTLS_PLATFORM_IS_UNIXLIKE`, after which `psa_crypto_random.c`
includes `<sys/time.h>` for `gettimeofday()`-based fork protection.
toy-os has no `fork()`, so that block is meaningless here as well as
unbuildable.

## Where the boundary is

**Our code lives in `userland/backends/mbedtls/`**, not here, so the
line between third-party and written-here is a directory boundary rather
than a convention -- the same arrangement `userland/backends/doom/` has.
That directory holds the configuration, the entropy source, the
monotonic clock, and the entropy-quality gate.
