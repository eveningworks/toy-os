#ifndef ULIB_UTLS_H
#define ULIB_UTLS_H

// A TLS client session over a connected socket.
//
// **THIS HEADER NAMES NO ENGINE.** It is implemented today by mbedTLS
// (`userland/backends/mbedtls/utls_mbedtls.c`, built into
// /lib/libssl.so) and mentions none of its types, so replacing the
// engine is a backend change rather than a change to every caller. That
// is not hypothetical tidiness: BearSSL was measured against mbedTLS
// before this was written, and the losing candidate was losing on
// maintenance rather than on fit.
//
// **NOT IN userland/lib/.** That directory is globbed wholesale into
// libuapp.a and libuapp.so, which every program links -- so a TLS call
// there would put mbedTLS behind every binary in the system. The
// implementation lives in libssl.so and a program opts in.
#include <stddef.h>

struct utls; // opaque

// How much the caller is prepared to trust this machine's randomness.
// A TLS private key drawn from TSC jitter under an emulator is not
// secret from anyone who can model the emulator, so utls_connect()
// REFUSES by default and this is the override. See
// `userland/backends/mbedtls/toyos_platform.c`.
enum utls_entropy_policy {
    UTLS_ENTROPY_REQUIRE_GOOD = 0, // refuse on jitter-only -- the default
    UTLS_ENTROPY_ALLOW_WEAK,       // proceed, and say so
};

struct utls_config {
    // The name to verify the certificate against, and to send as SNI.
    // REQUIRED even when verification is off: a shared-hosting server
    // given no name answers with the wrong certificate, and that reads
    // as a verification bug rather than a missing extension.
    const char *hostname;

    // Directory of PEM trust anchors. NULL means UTLS_DEFAULT_CA_DIR.
    const char *ca_dir;

    // Proceed with an UNVERIFIED peer. The connection is still
    // encrypted, and it is no longer authenticated: anyone able to
    // answer in the server's place can read and rewrite it. A caller
    // setting this should say so where a person will see it.
    int insecure;

    enum utls_entropy_policy entropy;
};

#define UTLS_DEFAULT_CA_DIR "/etc/ssl/certs"

// Is a TLS engine present in this build? Always 1 in libssl.so; exists
// so a caller linked against it can answer without a handshake.
int utls_available(void);

// Wraps an already-CONNECTED socket. The fd stays the caller's: this
// does not close it, on success or failure.
//
// Returns NULL on failure and writes a sentence into `err` -- which is
// the whole reason it is not an errno. A TLS failure has too many
// distinguishable causes to fold into one integer, and "certificate
// verification failed: not trusted by any anchor in /etc/ssl/certs" is
// what a caller needs to print.
struct utls *utls_connect(int fd, const struct utls_config *cfg,
                          char *err, size_t errcap);

// Both return the byte count, 0 at clean end of stream, or negative on
// error. A short return is normal and is NOT an error: one TLS record
// is what it is, and the kernel caps a socket read at SYS_NET_MSG_MAX
// (1472) regardless of the buffer handed down, so a 16 KiB record
// arrives over a dozen reads.
long utls_read(struct utls *t, void *buf, size_t n);
long utls_write(struct utls *t, const void *buf, size_t n);

// Sends close_notify, then frees. A TLS stream has an explicit end, and
// omitting it makes a truncation attack indistinguishable from a peer
// hanging up.
void utls_close(struct utls *t);

// "TLSv1.3" / "TLSv1.2", and the negotiated suite. Never NULL.
const char *utls_version(struct utls *t);
const char *utls_ciphersuite(struct utls *t);

#endif
