// The HID report-descriptor walker. See api/hid_parse.h for what it is
// for and what it deliberately is not.
//
// FREESTANDING, and compiled twice: once into the kernel and once for
// tools/hid_parse_hostcheck.py, which runs it against descriptors
// captured off real devices. So nothing kernel-only may appear here --
// no klog, no kmalloc, not even a string function.
#include "hid_parse.h"

// --- the item stream --------------------------------------------------
//
// A short item is one prefix byte -- bTag:4, bType:2, bSize:2 -- and 0,
// 1, 2 or 4 data bytes; bSize 3 means FOUR, which is the one encoding
// that surprises people reading it as a length. A long item (0xFE) has
// its own size byte and no defined tags; it is skipped.
#define ITEM_SIZE(prefix)  ((prefix) & 0x03)
#define ITEM_TYPE(prefix)  (((prefix) >> 2) & 0x03)
#define ITEM_TAG(prefix)   (((prefix) >> 4) & 0x0F)

#define TYPE_MAIN   0
#define TYPE_GLOBAL 1
#define TYPE_LOCAL  2

#define MAIN_INPUT            0x8
#define MAIN_COLLECTION       0xA
#define MAIN_END_COLLECTION   0xC

#define GLOBAL_USAGE_PAGE   0x0
#define GLOBAL_LOGICAL_MIN  0x1
#define GLOBAL_LOGICAL_MAX  0x2
#define GLOBAL_REPORT_SIZE  0x7
#define GLOBAL_REPORT_ID    0x8
#define GLOBAL_REPORT_COUNT 0x9

#define LOCAL_USAGE     0x0
#define LOCAL_USAGE_MIN 0x1
#define LOCAL_USAGE_MAX 0x2

#define PAGE_GENERIC_DESKTOP 0x01
#define PAGE_KEYBOARD        0x07
#define PAGE_BUTTON          0x09
#define PAGE_CONSUMER        0x0C

#define USAGE_POINTER   0x01
#define USAGE_MOUSE     0x02
#define USAGE_KEYBOARD  0x06
#define USAGE_X         0x30
#define USAGE_Y         0x31
#define USAGE_WHEEL     0x38
#define USAGE_AC_PAN    0x0238   // Consumer page

// An Input item's data byte: bit 0 Constant, bit 1 Variable, bit 2
// Relative. A CONSTANT field is padding and carries nothing; an ARRAY
// (Variable clear) is a list of usages that are down, which is what a
// keyboard's keycodes are and what makes them a different shape from a
// mouse's one-bit-per-button.
#define INPUT_CONSTANT 0x01
#define INPUT_VARIABLE 0x02

#define MAX_LOCAL_USAGES 8

static uint32_t item_data(const uint8_t *p, uint8_t size) {
    switch (size) {
    case 1:  return p[0];
    case 2:  return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    case 3:  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    default: return 0;
    }
}

// Sign-extended, for the logical minimum -- which is the only thing
// here whose sign matters, and the only way to know whether an axis is
// signed. `16 01 80` is -32767, not 32769.
static int32_t item_data_signed(const uint8_t *p, uint8_t size) {
    uint32_t v = item_data(p, size);
    switch (size) {
    case 1: return (int32_t)(int8_t)v;
    case 2: return (int32_t)(int16_t)v;
    case 3: return (int32_t)v;
    default: return 0;
    }
}

static void field_set(struct hid_field *f, uint16_t off, uint8_t bits,
                      uint8_t count, int is_signed) {
    // FIRST ONE WINS. A descriptor may describe the same usage in more
    // than one collection (the G305 names X and Y again under its
    // system-control collection); the mouse's own is the one already
    // taken, and overwriting it with a later one moves the axis to a
    // report the device never sends for motion.
    if (f->present) return;
    f->bit_off = off;
    f->bits = bits;
    f->count = count;
    f->is_signed = (uint8_t)(is_signed ? 1 : 0);
    f->present = 1;
}

int hid_parse_report_descriptor(const uint8_t *desc, uint32_t len,
                                int want_mouse, struct hid_layout *out) {
    if (!desc || !out) return 0;
    for (uint32_t i = 0; i < sizeof *out; i++) ((uint8_t *)out)[i] = 0;

    uint16_t usage_page = 0;
    int32_t  logical_min = 0;
    uint32_t report_size = 0, report_count = 0;
    uint8_t  report_id = 0;

    uint32_t local_usages[MAX_LOCAL_USAGES];
    uint8_t  local_count = 0;
    uint32_t usage_min = 0, usage_max = 0;
    int      have_range = 0;

    // The collection we are inside, and whether it is the one asked
    // for. Depth is tracked so the interesting collection ENDS -- a
    // descriptor with four top-level collections (this is what the
    // G305 has) would otherwise keep matching after its mouse ended.
    int depth = 0, want_depth = -1, matched = 0;
    uint16_t bit_off = 0;       // within the current report's body
    uint8_t  cur_id = 0;

    const uint16_t want_usage = want_mouse ? USAGE_MOUSE : USAGE_KEYBOARD;

    uint32_t i = 0;
    while (i < len) {
        uint8_t prefix = desc[i++];
        if (prefix == 0xFE) {                 // long item: skip it whole
            if (i + 1 >= len) break;
            uint8_t dsize = desc[i];
            i += 2;
            if (dsize > len - i) break;
            i += dsize;
            continue;
        }
        uint8_t size = ITEM_SIZE(prefix);
        uint8_t nbytes = (size == 3) ? 4 : size;
        if (nbytes > len - i) break;          // truncated -- stop, do not guess
        const uint8_t *data = desc + i;
        i += nbytes;

        switch (ITEM_TYPE(prefix)) {
        case TYPE_GLOBAL:
            switch (ITEM_TAG(prefix)) {
            case GLOBAL_USAGE_PAGE:   usage_page = (uint16_t)item_data(data, nbytes); break;
            case GLOBAL_LOGICAL_MIN:  logical_min = item_data_signed(data, nbytes); break;
            case GLOBAL_LOGICAL_MAX:  break;
            case GLOBAL_REPORT_SIZE:  report_size = item_data(data, nbytes); break;
            case GLOBAL_REPORT_COUNT: report_count = item_data(data, nbytes); break;
            case GLOBAL_REPORT_ID:
                report_id = (uint8_t)item_data(data, nbytes);
                // EACH REPORT ID IS ITS OWN REPORT, so the bit offset
                // starts again. Carrying it across is how every field
                // after the first ID ends up read from the wrong place.
                if (report_id != cur_id) { cur_id = report_id; bit_off = 0; }
                // THE ID USUALLY ARRIVES AFTER ITS COLLECTION OPENS --
                // `05 01 09 02 a1 01 85 02` is the ordinary shape, so
                // reading it only at the collection catches a 0 that
                // is never corrected, and every report is then decoded
                // one byte out of step.
                if (matched && out->report_id == 0) out->report_id = report_id;
                break;
            default: break;
            }
            break;

        case TYPE_LOCAL:
            switch (ITEM_TAG(prefix)) {
            case LOCAL_USAGE:
                if (local_count < MAX_LOCAL_USAGES)
                    local_usages[local_count++] = item_data(data, nbytes);
                break;
            case LOCAL_USAGE_MIN: usage_min = item_data(data, nbytes); have_range = 1; break;
            case LOCAL_USAGE_MAX: usage_max = item_data(data, nbytes); have_range = 1; break;
            default: break;
            }
            break;

        case TYPE_MAIN:
            switch (ITEM_TAG(prefix)) {
            case MAIN_COLLECTION:
                // The TOP-LEVEL usage is what names a collection. Taken
                // from the local usage queue as it stood when the
                // collection opened, which is where `05 01 09 02 a1 01`
                // puts it.
                if (depth == 0 && local_count > 0 &&
                    usage_page == PAGE_GENERIC_DESKTOP &&
                    (local_usages[0] & 0xFFFF) == want_usage) {
                    matched = 1;
                    want_depth = depth;
                    out->report_id = report_id;
                    bit_off = 0;
                }
                depth++;
                break;

            case MAIN_END_COLLECTION:
                if (depth > 0) depth--;
                if (matched && depth == want_depth) {
                    // The collection we wanted has closed. Everything
                    // after it belongs to another report.
                    matched = 0;
                    want_depth = -1;
                }
                break;

            case MAIN_INPUT: {
                uint32_t flags = item_data(data, nbytes);
                uint32_t bits = report_size * report_count;
                // CLAMPED, because these are a device's numbers: a
                // report cannot be bigger than the biggest thing a
                // packet can hold, and a silly count must not overflow
                // the offset it is added to.
                if (report_size > 32 || report_count > 512 || bits > 4096) {
                    bit_off = 0xFFFF;         // poison: nothing after this is usable
                    break;
                }

                if (matched && !(flags & INPUT_CONSTANT)) {
                    int is_var = (flags & INPUT_VARIABLE) != 0;
                    uint32_t u0 = local_count ? local_usages[0] : 0;

                    if (usage_page == PAGE_BUTTON && is_var && have_range) {
                        uint32_t n = usage_max >= usage_min
                                   ? usage_max - usage_min + 1 : 0;
                        if (n > report_count) n = report_count;
                        if (n > 255) n = 255;
                        field_set(&out->buttons, bit_off, (uint8_t)report_size,
                                  (uint8_t)n, 0);
                    } else if (usage_page == PAGE_GENERIC_DESKTOP && is_var) {
                        // Each usage in the queue takes ONE element, in
                        // order: `09 30 09 31 ... 95 02` is X then Y,
                        // not two of X.
                        for (uint32_t k = 0; k < local_count && k < report_count; k++) {
                            uint16_t off = (uint16_t)(bit_off + k * report_size);
                            switch (local_usages[k] & 0xFFFF) {
                            case USAGE_X:
                                field_set(&out->x, off, (uint8_t)report_size, 1,
                                          logical_min < 0); break;
                            case USAGE_Y:
                                field_set(&out->y, off, (uint8_t)report_size, 1,
                                          logical_min < 0); break;
                            case USAGE_WHEEL:
                                field_set(&out->wheel, off, (uint8_t)report_size, 1,
                                          logical_min < 0); break;
                            default: break;
                            }
                        }
                    } else if (usage_page == PAGE_CONSUMER && is_var &&
                               (u0 & 0xFFFF) == USAGE_AC_PAN) {
                        field_set(&out->pan, bit_off, (uint8_t)report_size, 1,
                                  logical_min < 0);
                    } else if (usage_page == PAGE_KEYBOARD) {
                        if (is_var) {
                            // The modifier byte: eight one-bit usages
                            // E0..E7. Anything else variable on this
                            // page is an LED-style field we do not read.
                            if (have_range && usage_min == 0xE0)
                                field_set(&out->mods, bit_off, (uint8_t)report_size,
                                          (uint8_t)(report_count > 255 ? 255 : report_count), 0);
                        } else {
                            // AN ARRAY: `count` slots, each holding the
                            // USAGE of a key that is down. Six of them
                            // is the familiar six-key rollover.
                            field_set(&out->keys, bit_off, (uint8_t)report_size,
                                      (uint8_t)(report_count > 255 ? 255 : report_count), 0);
                        }
                    }
                }

                if (bit_off != 0xFFFF) {
                    // EVERY Input advances the offset, matched or not,
                    // constant or not. A field this parser has no use
                    // for still occupies its bits, and skipping the
                    // advance moves everything after it.
                    uint32_t next = (uint32_t)bit_off + bits;
                    bit_off = next > 0xFFFE ? 0xFFFF : (uint16_t)next;
                    if (matched && bit_off != 0xFFFF && bit_off > out->report_bits)
                        out->report_bits = bit_off;
                }
                break;
            }
            default: break;
            }
            // A Main item consumes the local state, always -- including
            // the ones this switch ignores. Leaving a stale usage queue
            // behind is how a later field inherits an earlier one's
            // identity.
            local_count = 0;
            usage_min = usage_max = 0;
            have_range = 0;
            break;

        default: break;
        }
    }

    if (want_mouse) {
        // AXES ARE THE TEST. A mouse collection with no X and Y was not
        // understood, whatever else was found, and using it would put
        // the pointer somewhere the device never said.
        out->is_mouse = (uint8_t)(out->x.present && out->y.present);
        return out->is_mouse;
    }
    out->is_keyboard = (uint8_t)(out->keys.present || out->mods.present);
    return out->is_keyboard;
}

int32_t hid_field_read(const struct hid_field *f, const uint8_t *body,
                       uint32_t body_len, uint8_t index) {
    if (!f || !f->present || !body || index >= f->count) return 0;
    uint32_t off = (uint32_t)f->bit_off + (uint32_t)index * f->bits;
    if (f->bits == 0 || f->bits > 32) return 0;
    if (off + f->bits > body_len * 8u) return 0;   // the report is shorter than it claimed

    uint32_t v = 0;
    for (uint8_t b = 0; b < f->bits; b++) {
        uint32_t bit = off + b;
        if (body[bit >> 3] & (1u << (bit & 7))) v |= (1u << b);
    }
    if (f->is_signed && f->bits < 32 && (v & (1u << (f->bits - 1))))
        v |= ~((1u << f->bits) - 1u);          // sign-extend
    return (int32_t)v;
}
