// See kcrc.h.
#include "kcrc.h"

uint32_t kcrc32_update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            // Branchless: mask is all-ones when the low bit is set.
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

uint32_t kcrc32(const void *data, size_t len) {
    return KCRC32_FINAL(kcrc32_update(KCRC32_INIT, data, len));
}
