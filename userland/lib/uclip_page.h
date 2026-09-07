#ifndef ULIB_UCLIP_PAGE_H
#define ULIB_UCLIP_PAGE_H

#include <stdint.h>

// The clipboard's shared page: the contract between every app's
// lib/uclip.c and /bin/clipboardd, and between no other two things.
//
// **IT IS NOT IN kernel/include/abi/ ON PURPOSE.** That directory is
// the kernel<->userland contract, and the kernel has no part in this
// any more: the clipboard is a named shared-memory object created by a
// ring-3 service, so this is a userland<->userland contract and lives
// with the library that speaks it.
//
// WHY A SERVICE AND NOT THE KERNEL. It used to be a buffer inside
// kernel/proc/win_server.c, reached by a syscall. That put untrusted
// user data and a piece of DESKTOP POLICY -- what a clipboard is,
// what a cut means -- in ring 0, which is the one thing CLAUDE.md says
// ring 0 must not contain. Every system that has thought about it
// agrees: macOS has `pboard`, Android's ClipboardManager lives in
// system_server, and Windows' copy-into-system-memory design is the
// outlier that sat in win32k.sys and has been moving out ever since.
// `/bin/soundd` is this repository's own worked example of the shape.
//
// WHY THE DATA PATH HAS NO SERVER IN IT. The daemon does not carry
// bytes. It CREATES the object and owns its lifetime -- which is the
// whole reason the clipboard survives every app exiting, and the
// original argument for keeping it in the kernel -- and it breaks a
// lock whose owner died. Clients read and write the page directly, so
// a paste costs no syscall and no context switch, and an app that
// wants to grey out its Paste item can watch `serial` for free.
//
// THE BEACON IS THE OBJECT. There is no separate "is it running" name:
// opening CLIP_SHM_NAME either works, or the service is not up.

#define CLIP_SHM_NAME "clipboard"

#define CLIP_BYTES 65536 // the packed payload's cap
#define CLIP_MAX   64    // and how many entries it may name

#define CLIP_OP_NONE 0
#define CLIP_OP_COPY 1
#define CLIP_OP_CUT  2

#define CLIP_KIND_FILES 0
#define CLIP_KIND_TEXT  1

// Written LAST when the daemon initialises the page, and checked by
// every client: a mapping whose magic is unset is one whose other
// fields have not been written yet.
#define CLIP_MAGIC 0x50494C43u // 'CLIP'

// A writer that cannot take the lock this many spins gives up and
// REFUSES the copy. Contention is a memcpy wide, so reaching this at
// all means the holder is stuck rather than slow.
#define CLIP_LOCK_SPINS 200000

// And the daemon breaks a lock held longer than this, because the only
// way one stays taken is an owner that died inside it.
#define CLIP_LOCK_STALE_MS 2000

struct clip_page {
    uint32_t magic;

    // SEQLOCK. Odd while a writer is mid-update; a reader takes it
    // before and after its copy and retries if the two differ. That is
    // what makes a paste lock-free and syscall-free -- readers never
    // block a writer and never block each other, which matters because
    // reading is what happens on every frame that greys a menu item.
    uint32_t seq;

    // WRITER MUTUAL EXCLUSION, separate from the seqlock because a
    // seqlock alone assumes ONE writer and any app may copy. Holds the
    // owner's pid rather than a flag, so a stuck lock names who stuck
    // it in the daemon's log.
    uint32_t lock;
    uint32_t lock_pid;
    uint64_t lock_ms;   // monotonic ms at which it was taken

    uint32_t op;        // CLIP_OP_*
    uint32_t kind;      // CLIP_KIND_*
    uint32_t count;     // entries packed into `data`
    uint32_t len;       // bytes of `data` in use, the NULs included

    // BUMPED ON EVERY COMMIT, and never reused. It is how a client
    // notices that somebody else replaced the clipboard underneath it,
    // and comparing payloads to decide that would be both slower and
    // wrong -- copying the same file twice is a real change to the
    // cut/copy mode.
    uint32_t serial;

    // `count` NUL-terminated strings packed end to end: absolute paths
    // when the kind is FILES, one run of text (count 1, its NUL
    // included in `len`) when it is TEXT.
    char data[CLIP_BYTES];
};

#endif // ULIB_UCLIP_PAGE_H
