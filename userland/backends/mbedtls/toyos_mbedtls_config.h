#ifndef TOYOS_MBEDTLS_CONFIG_H
#define TOYOS_MBEDTLS_CONFIG_H

// toy-os's mbedTLS configuration -- what a TLS CLIENT needs, and
// nothing else.
//
// Written from scratch rather than by editing upstream's default
// config, which enables DES, Camellia, ARIA, DTLS, the whole server
// side and every CBC cipher suite. `--gc-sections` would drop most of
// that from the binary, but not from the HANDSHAKE: a suite this file
// enables is a suite the client offers and will accept, so the enabled
// set IS the security posture and belongs somewhere a person can read
// it.
//
// **NOTHING IN userland/ports/mbedtls/ IS EDITED.** Every choice here
// is a macro, which is the vendoring rule -- flags and configuration,
// never patches.
//
// The build also passes `-nostdinc` and undefines `__unix__`/`__linux__`
// (see the Makefile), because GCC predefines those for every toy-os
// compile and mbedTLS otherwise concludes it is on Linux and reaches
// for <sys/time.h> and the host's sockets.

// --- what the platform provides ------------------------------------
//
// tolibc has stdio, so the default platform layer is used as-is. The
// three things it cannot supply are time, entropy and a filesystem.

#define MBEDTLS_PLATFORM_C

// toy-os has no gettimeofday and no time_t source mbedTLS knows about,
// so both clocks are ours. Certificate expiry needs a real wall clock
// (SYS_GETTIME); the TLS 1.3 handshake needs a millisecond monotonic
// one (SYS_MONOTONIC_NS), and they are NOT the same clock -- a wall
// clock can step backwards over NTP mid-handshake.
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT

// No /dev/urandom and no getrandom() by mbedTLS's reckoning: entropy
// comes from SYS_GETRANDOM through mbedtls_hardware_poll() in
// toyos_platform.c, which also refuses to hand out a key when
// QUERY_RANDOM says the source is TSC jitter.
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

// MBEDTLS_FS_IO is deliberately OFF. It exists for
// mbedtls_x509_crt_parse_file()/_path(), which want stat() and
// dirent to walk a certificate DIRECTORY; the trust store is read by
// our own code (uhttp reads /etc/ssl/certs itself) and handed over as
// a buffer, so the library never touches a path.

// --- the entropy and key material ----------------------------------

#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_USE_PSA_CRYPTO   // TLS 1.3 is built on PSA in 3.6

// --- symmetric: AEAD ONLY ------------------------------------------
//
// No CBC and no stream cipher other than ChaCha20. Every suite this
// client offers is authenticated encryption, which removes the
// MAC-then-encrypt padding-oracle family (Lucky13 and its descendants)
// by construction rather than by careful implementation.
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C
// AES-NI and PCLMULQDQ through mbedTLS's own inline assembly, chosen at
// run time by CPUID, so a CPU without them (QEMU's default model) keeps
// the C tables. Without it a VNC frame over TLS cost twice the plain one
// on the ASUS. HAVE_ASM also gives bignum its x86-64 multiply.
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_AESNI_C

// --- hashes ---------------------------------------------------------
//
// SHA-1 is enabled because X.509 parsing still meets it in the wild --
// in a chain's algorithm identifiers, and in key identifiers, which are
// hashes rather than signatures. Whether a SHA-1 SIGNATURE is acceptable
// is not this file's decision: it belongs to the certificate profile the
// client installs at runtime, and that profile refuses it.
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_MD_C
#define MBEDTLS_HKDF_C

// --- public key -----------------------------------------------------
//
// RSA for signature VERIFICATION only (the web is still mostly RSA
// certificates); ECDSA for the rest. No RSA key exchange: a server's
// RSA key never encrypts a secret here, so a compromised server key
// cannot decrypt recorded traffic. Forward secrecy on every connection.
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21        // RSA-PSS, which TLS 1.3 requires

// AND THE ONE THAT ACTUALLY PUTS PSS ON THE WIRE. MBEDTLS_PKCS1_V21
// gives the library the algorithm; this is what adds
// rsa_pss_rsae_sha256/384/512 to the signature_algorithms extension
// (ssl_tls.c gates that list on THIS macro, not on PKCS1_V21). TLS 1.3
// forbids PKCS#1 v1.5 in CertificateVerify, so without it a client
// offers only rsa_pkcs1_* for RSA and every TLS 1.3 server holding an
// RSA certificate answers with a handshake_failure alert -- a
// configuration that compiles, links and cannot talk to most of the web.
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C

// X25519 first: it is the modern default and the one curve here whose
// implementation has no point-validation footguns. The two NIST curves
// follow because a server that offers only P-256 is still common.
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM

// --- X.509 ----------------------------------------------------------
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C      // /etc/ssl/certs holds PEM

// --- TLS ------------------------------------------------------------
//
// Client only. There is no server here: /bin/httpd speaks plaintext
// and giving it TLS would mean a private key on disk and a decision
// about where it lives, which is a separate piece of work.
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
// THE SERVER HALF, for /bin/remoted's VeNCrypt: a key and a self-signed
// certificate made on this machine (utls_server_identity()).
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_PEM_WRITE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C
// DETERMINISTIC SIGNATURES (RFC 6979), so the same key and the same names
// make the same certificate byte for byte -- the fingerprint a viewer
// remembered survives every reboot.
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_ECDSA_DETERMINISTIC
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

// SNI. Not optional in practice: a shared-hosting server given no name
// answers with the wrong certificate, and the failure looks like a
// verification bug rather than a missing extension.
#define MBEDTLS_SSL_SERVER_NAME_INDICATION

// TLS 1.3 requires the peer's certificate to be KEPT rather than
// verified and dropped: its post-handshake authentication and the
// resumption path both need to refer back to it. Not optional --
// check_config.h refuses the combination.
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

// The record buffers are the client's largest allocation. A TLS record
// is up to 16 KiB inbound and must be accepted at that size, but this
// client's own requests are a few hundred bytes, so the outbound buffer
// is cut to 4 KiB. Both are heap, never stack -- ring 3 warns above a
// 2 KiB frame and the guard page is one page.
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

// --- diagnostics ----------------------------------------------------
//
// ERROR_C turns a negative code into a sentence. It costs a string
// table and is worth it: a TLS failure a user cannot read is a bug
// report nobody can act on.
#define MBEDTLS_ERROR_C

// NO #include "mbedtls/check_config.h" HERE. build_info.h includes it
// itself, and only AFTER the config_adjust_*.h headers have derived
// MBEDTLS_MD_CAN_* and PSA_WANT_* from the macros above. Including it
// from this file checks prerequisites that have not been computed yet,
// and reports a correct configuration as missing half of itself.

#endif
