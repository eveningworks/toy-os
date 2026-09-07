// The three things mbedTLS cannot get for itself on toy-os: entropy, a
// millisecond monotonic clock, and an answer to "is this machine's
// randomness good enough to key a connection with?".
//
// This file is OURS. Nothing in userland/ports/mbedtls/ is edited --
// see that directory's README for why the boundary is a directory
// rather than a convention.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mbedtls/build_info.h"
#include "mbedtls/entropy.h"

#include "rt/sys.h"
#include "toyos_tls_entropy.h"

// SYS_GETRANDOM never returns a short count -- it fills the buffer or
// fails -- so a partial read is a real error rather than something to
// loop over. The cap is SYS_GETRANDOM_MAX; mbedTLS asks in blocks well
// under it, but the loop is here because the ABI's limit is the ABI's,
// not this caller's to assume.
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    size_t done = 0;
    while (done < len) {
        size_t want = len - done;
        if (want > SYS_GETRANDOM_MAX) want = SYS_GETRANDOM_MAX;
        int got = sys_getrandom(output + done, want);
        if (got != (int)want) {
            *olen = done;
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }
        done += want;
    }
    *olen = done;
    return 0;
}

// TLS 1.3 measures elapsed time, never dates, so this must be the
// MONOTONIC clock and not the wall clock -- `time()` can step backwards
// when ntpd corrects it, and a handshake that sees time run backwards
// mid-exchange misjudges every timeout it has. Certificate expiry is
// the opposite case and correctly uses tolibc's `time()`, which is why
// the two are not the same call.
mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)(sys_monotonic_ns() / 1000000ULL);
}

// WHY A CALLER IS MADE TO ASK.
//
// SYS_GETRANDOM always answers and never says how good the answer is --
// deliberately, because a ring-3 program handed a quality flag "would
// mostly use it to decide to carry on anyway" (abi/syscall_abi.h). That
// reasoning holds for the general case and breaks for exactly one:
// a private key. Under an emulator with no virtio-rng the source is TSC
// jitter, where the "hardware" being timed is itself software, and a
// key drawn from it is not secret from anyone who can model the
// emulator.
//
// So the quality is read from QUERY_RANDOM, which exists for precisely
// this -- the fact, separate from the bytes -- and the decision is left
// to the caller rather than taken here. `wget` refuses; a future
// caller with a different threat model may not.
enum toyos_entropy_grade toyos_entropy_grade(char *name, size_t cap) {
    struct query_random r;
    if (name && cap) name[0] = '\0';

    if (sys_query_record(QUERY_RANDOM, 0, &r, sizeof r) < (int)sizeof r)
        return TOYOS_ENTROPY_UNKNOWN;

    if (name && cap) {
        r.name[sizeof r.name - 1] = '\0';
        strlcpy(name, r.name, cap);
    }

    // QUERY_RANDOM_* is ordered by trust, so this is a threshold rather
    // than a list of acceptable values -- a source added later that
    // ranks above virtio needs no change here.
    if (r.quality >= QUERY_RANDOM_VIRTIO) return TOYOS_ENTROPY_GOOD;
    if (r.quality == QUERY_RANDOM_JITTER) return TOYOS_ENTROPY_WEAK;
    return TOYOS_ENTROPY_NONE;
}
