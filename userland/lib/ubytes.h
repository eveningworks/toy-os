#ifndef ULIB_UBYTES_H
#define ULIB_UBYTES_H

// Little-endian fields out of a byte buffer, and a read at an offset
// that does not come back short -- what every parser of an on-disk
// format here starts by writing (zip, WAD, RIFF/WAV, SoundFont).
//
// Byte by byte, never a cast of the pointer: the fields are unaligned
// as often as not, and the value must not depend on the host's order.
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

static inline uint16_t ub_le16(const void *v) {
    const uint8_t *p = (const uint8_t *)v;
    return (uint16_t)(p[0] | p[1] << 8);
}

static inline uint32_t ub_le32(const void *v) {
    const uint8_t *p = (const uint8_t *)v;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// `n` bytes at `off`, all of them: 0, or -1 for a seek that missed, an
// error, or the end of the file first.
static inline int ub_read_at(int fd, unsigned long off, void *buf, size_t n) {
    if (lseek(fd, (off_t)off, SEEK_SET) != (off_t)off) return -1;
    size_t got = 0;
    while (got < n) {
        size_t want = n - got > (1u << 20) ? (1u << 20) : n - got;   // a MiB a call
        long r = read(fd, (char *)buf + got, want);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

#endif
