#ifndef KCRC_H
#define KCRC_H

#include <stddef.h>
#include <stdint.h>

// CRC-32, the REFLECTED IEEE 802.3 / zlib form (polynomial 0xEDB88320).
// This is what zip, gzip, PNG and a GPT header all mean by "CRC32", and
// it is NOT what POSIX `cksum` computes -- that one runs 0x04C11DB7
// unreflected and feeds the length in at the end, so the two disagree on
// every input. `/bin/sum -a crc32` prints this one.
//
// Table-less, a bit at a time. The GPT header is ~92 bytes, and the one
// caller that hashes whole files is bounded by the disk.

uint32_t kcrc32(const void *data, size_t len);

// The running form, for data that never exists as one buffer -- the GPT
// entry array is 16 KiB and is fed through a sector at a time. Start at
// KCRC32_INIT, end with KCRC32_FINAL.
#define KCRC32_INIT 0xFFFFFFFFu
#define KCRC32_FINAL(crc) (~(uint32_t)(crc))

uint32_t kcrc32_update(uint32_t crc, const void *data, size_t len);

#endif // KCRC_H
