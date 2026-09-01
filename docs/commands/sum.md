# sum

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

```
sum [-a ALGORITHM] [-c LISTFILE] [FILE...]
```

## Description

Prints a checksum or digest of each `FILE`, or of standard input when
no file is named (or when the file is `-`). `-a` picks the algorithm;
without it, `crc32`.

```
/$ sum /bin/hello
3637643924 119952 /bin/hello

/$ sum -a sha256 /bin/hello
f40c609d804d317b951ea4f2b2555aa2cce87d532e36ea1ef05ad10a7093aff3  /bin/hello

/$ cat /etc/toyos.conf | sum
3830823995 73 -
```

**The algorithms are a table, not a program each.** Today it holds
`crc32` and `sha256`; `sum -a` with a name it does not know prints the
list rather than guessing. That is coreutils 9.0's `cksum -a` shape,
which replaced `md5sum`/`sha1sum`/`sha256sum` for the reason this
project keeps applying to C: a binary per algorithm multiplies.

**The line shape is the algorithm's own**, so the host can check this
OS's work and the other way round:

| algorithm | line | what prints it on Linux |
|---|---|---|
| `crc32` | `<decimal CRC> <bytes> <name>` | `cksum -a crc32b` |
| `sha256` | `<hex>  <name>` (two spaces) | `sha256sum` |

**`-c LISTFILE` verifies instead of printing.** Each line is read in the
shape above, the named file is re-hashed, and each result is reported:

```
/$ sum /bin/hello /bin/echo > /tmp/list
/$ sum -c /tmp/list
/bin/hello: OK
/bin/echo: OK
```

A mismatch prints `<name>: FAILED`, a file that cannot be opened prints
`<name>: FAILED open or read`, and either makes the exit status 1. Lines
that are blank or start with `#` are skipped. A line that is not this
algorithm's shape is counted and reported rather than guessed at -- and
if no line in the file was usable, that is an error too, not a clean
run: a manifest in the wrong format would otherwise look exactly like a
manifest that all passed.

In `crc32`'s shape the **size is half the check**. `-c` compares it as
well as the CRC, which is why the two-field line is worth keeping.

## The `crc32` here is not `cksum`'s

`sum -a crc32` computes the **reflected IEEE 802.3 / zlib CRC-32**
(polynomial `0xEDB88320`) -- what zip, gzip, PNG, Ethernet and a GPT
header all mean by "CRC32". Plain POSIX `cksum` computes a *different*
CRC-32: unreflected over `0x04C11DB7`, with the length fed in at the
end. **The two agree on nothing.** Check against `cksum -a crc32b`
(coreutils 9.6 and later) or `python3 -c 'import zlib'`, not against a
bare `cksum`.

That choice is deliberate: this is the same function
`kernel/lib/kcrc.c` computes for GPT headers, so one polynomial serves
ring 0 and ring 3 and there is no second implementation to drift.

## What it is not

Not a MAC and not a password hash. SHA-256 is here to answer "did these
bytes survive the trip"; toy-os has no crypto, no key management and no
constant-time anything, and nothing here should be trusted against an
adversary who can choose the input.

There is no `-b`/`-t` mode: every file is read as bytes, because this OS
has no text/binary distinction to have a flag about.

## Where the code lives

The algorithms are **`/lib/libhash.so`** (`userland/dynlib/uhash.c`,
public header `<uhash.h>`), so `sum` is a thin front end and any other
program can link the same code -- it is the first shared library here
that exists to be used rather than to prove the loader works. The CRC
comes from `kernel/lib/kcrc.c`, compiled a second time `-fpic` into that
library.

## See also

[`stat`](stat.md) for a file's size and timestamps, [`cat`](cat.md) for
the bytes themselves, [`cp`](cp.md) for making the copy you are about to
check.
