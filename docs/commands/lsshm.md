# lsshm

**a `/bin` program.**

**Category:** Developer and diagnostic (`help tests`)

## Synopsis

    lsshm

## Description

Every shared-memory object that exists right now: its name, its size,
how many references it has, and the pid that created it.

    NAME                            BYTES  REFS  CREATOR
    snd.server                       4096     1  11
    snd.14                          69632     4  14

**A namespace nothing can list is a namespace you reason about instead
of reading**, which is the whole reason this exists. Shared memory here
is reached by NAME (`SYS_SHM_OPEN`, `docs/decisions.md`), so a server's
clients *are* rows in this table — `/bin/soundd` names each client's
ring `snd.<pid>` — and "which programs is the daemon mixing, and did a
dead one leave its ring behind?" is one command rather than an
inference from a log.

**A row named `(anon)` is a NAMELESS object** — one a kernel subsystem
made for itself, which nothing can reach through `SYS_SHM_OPEN` because
it has no name to pass. Every open window's pixels are one of these
(two per window, one per buffer), and they are nameless on purpose: this
namespace has no permissions, so a name would be a way for any process
to map somebody else's window.

It earned its place immediately: a stalled audio client was diagnosed
from two rows carrying the SAME name, one of them `(unlinked)`, which
is what a stale object plus a recycled pid looks like.

## Reading a row

`REFS` counts open descriptors **plus** live mappings — a client that
has mapped its own ring and had it mapped by a server reads 4. An
object with `(unlinked)` has been removed from the namespace and is
kept alive only by whoever still holds it; nothing new can open that
name, and the frames go at the last reference.

`CREATOR` is the pid that made it. **When that process dies the name is
released at once**, even though the frames may live on, so a row whose
creator is not in `ps` should not normally appear.

## See also

`soundd`, `ps`, `meminfo`.
