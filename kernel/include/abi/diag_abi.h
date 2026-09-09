#ifndef DIAG_ABI_H
#define DIAG_ABI_H

#include <stdint.h>

// THE DIAGNOSTIC REGISTRY: asking a ring-3 SERVICE a question from
// either ring, by NAME.
//
// This was the window server's, hardwired to the compositor
// (`WIN_REQ_DEBUG_*`, now retired). Everything about it except the
// endpoint was already generic -- one command at a time, an owner, a
// lapse deadline, a reply chunked back -- and the desktop stopped being
// the only ring-3 service worth interrogating some time ago:
// `/bin/soundd`, `/bin/netd`, `/bin/clipboardd` and init are all
// processes with state a person may want to read. So the endpoint is a
// registered NAME and the machinery is written once.
//
// D-Bus introspection and a per-daemon control socket are the two shapes
// real systems use; this is the second, with the kernel holding the
// name table because the one caller that cannot be a ring-3 client is
// the kernel's own serial console -- and reaching a wedged service when
// no shell is available is the whole reason a kernel-side path exists at
// all. See docs/decisions.md.

#define DIAG_NAME_LEN  16   // longest provider name, including the NUL
#define DIAG_CMD_LEN  128   // longest command, including the NUL
#define DIAG_CHUNK    512   // reply bytes per message, excluding the NUL

// The whole reply a provider may hand back, held by the kernel between
// the provider that formatted it and the caller draining it. In the ABI
// because both ends size a buffer by it.
#define DIAG_REPLY_MAX 16384

// --- message types ----------------------------------------------------

// A CLIENT asks: `name` is the provider, `text` the command. Answers
// with the first chunk, or DIAG_F_PENDING when the provider has to be
// woken and asked.
#define DIAG_CMD      1
// ...and drains the rest, one chunk per call. No inputs.
#define DIAG_MORE     2

// A PROVIDER claims `name`. Idempotent for the same pid; refused when
// somebody else holds the name and is still alive.
#define DIAG_CLAIM    3
// ...and releases it. A provider that dies is dropped anyway
// (diag_provider_gone), so this is for an orderly exit.
#define DIAG_RELEASE  4

// A PROVIDER fetches the command it was woken for, into `text`. Answers
// 0 when there is nothing pending, which is the normal case for a wake
// that was about something else.
#define DIAG_TAKE     5
// ...and appends one chunk of its answer, DIAG_F_MORE set on every piece
// but the last. Cleared by the last one, which is what releases the
// waiting caller.
#define DIAG_REPLY    6

// What a reply carries back.
#define DIAG_OUT      7

// --- flags ------------------------------------------------------------

// More chunks follow: ask again with DIAG_MORE. The reply is NOT
// self-delimiting -- a chunk that exactly fills `text` cannot otherwise
// be told from a truncated one.
#define DIAG_F_MORE     0x01
// The provider did not recognise the command; `text` holds nothing. Kept
// distinct from an empty reply, because a command that legitimately
// prints nothing must not read as a typo.
#define DIAG_F_UNKNOWN  0x02
// No answer yet -- the provider has been woken and has not replied. Poll
// with DIAG_MORE. A caller that treats this as "done" prints nothing and
// reports a working service as silent.
#define DIAG_F_PENDING  0x04

// `pid` for a request originating in the kernel itself rather than in a
// scheduled process -- the serial debug console is the only such client.
// Distinct from 0, which the syscall path already refuses.
#define DIAG_PID_KERNEL (-1)

struct diag_msg {
    uint32_t type;   // DIAG_* going in, DIAG_OUT coming back
    uint32_t flags;  // DIAG_F_*, reply only
    uint32_t len;    // bytes valid in `text`, reply only
    uint32_t reserved; // must be 0; keeps the struct 8-byte aligned

    // WHICH PROVIDER. Read on DIAG_CMD, DIAG_CLAIM and DIAG_RELEASE, and
    // ignored on the rest -- a drain belongs to the caller that started
    // it, so naming the provider again would be a second chance to name
    // a different one.
    char name[DIAG_NAME_LEN];

    char text[DIAG_CHUNK + 1]; // command in / reply chunk out, NUL-terminated
};

#endif
