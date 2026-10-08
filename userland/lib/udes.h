#ifndef ULIB_UDES_H
#define ULIB_UDES_H

#include <stdint.h>

// DES (FIPS 46-3), one 64-bit block at a time -- for the protocols that
// still require it, which here means VNC's password check (RFB 7.2.2).
//
// **NOT FOR ANYTHING NEW.** DES has a 56-bit key and fell to brute force
// in 1998; it is here because a VNC viewer encrypts its challenge with
// it and nothing else will do. Use TLS (mbedtls) for confidentiality.
//
// The key is 8 bytes with the parity bits ignored, as every
// implementation does. Allocates nothing; the schedule is the caller's.
// tools/des_hostcheck.py checks it against OpenSSL.

struct udes_key { uint64_t sub[16]; };   // the 16 round keys, 48 bits each

void udes_set_key(struct udes_key *k, const uint8_t key[8]);
void udes_encrypt(const struct udes_key *k, const uint8_t in[8], uint8_t out[8]);
void udes_decrypt(const struct udes_key *k, const uint8_t in[8], uint8_t out[8]);

#endif
