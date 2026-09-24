#include "multiboot.h"
#include "vga.h"
#include "string.h"
#include <stdint.h>

static uint64_t mb_info_addr = 0;

void multiboot_set_info(uint64_t addr) {
    mb_info_addr = addr;
}

int multiboot_get_info_range(uint64_t *out_start, uint64_t *out_end) {
    if (mb_info_addr == 0) return 0;
    uint32_t total_size = *(uint32_t *)(uintptr_t)mb_info_addr;
    *out_start = mb_info_addr;
    *out_end = mb_info_addr + total_size;
    return 1;
}

struct mb_tag {
    uint32_t type;
    uint32_t size;
};

struct mb_tag_basic_meminfo {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;
    uint32_t mem_upper;
};

struct mb_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

struct mb_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb_mmap_entry entries[];
};

static const char *mmap_type_name(uint32_t type) {
    switch (type) {
        case 1: return "available";
        case 3: return "ACPI reclaimable";
        case 4: return "reserved (hibernate)";
        case 5: return "defective";
        default: return "reserved";
    }
}

// Was a local hex digit loop -- the fourth copy of vga_write_hex()'s
// body in the tree. vga_write_hex() itself does exactly this now (both
// go through knum.h), so the local one is gone.
static void write_hex64(uint64_t v) {
    vga_write_hex(v);
}

struct mb_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
    // followed by color-type-specific palette/mask info
};

int multiboot_get_framebuffer(struct framebuffer_info *out) {
    out->found = 0;
    if (mb_info_addr == 0) return 0;

    uint8_t *base = (uint8_t *)(uintptr_t)mb_info_addr;
    uint32_t total_size = *(uint32_t *)base;
    uint8_t *ptr = base + 8;
    uint8_t *end = base + total_size;

    while (ptr < end) {
        struct mb_tag *tag = (struct mb_tag *)ptr;
        if (tag->type == 0) break;

        if (tag->type == 8) {
            struct mb_tag_framebuffer *fb = (struct mb_tag_framebuffer *)tag;
            out->addr = fb->framebuffer_addr;
            out->pitch = fb->framebuffer_pitch;
            out->width = fb->framebuffer_width;
            out->height = fb->framebuffer_height;
            out->bpp = fb->framebuffer_bpp;
            out->type = fb->framebuffer_type;

            if (fb->framebuffer_type == 1) {
                // RGB color info immediately follows the fixed header
                uint8_t *p = (uint8_t *)fb + sizeof(struct mb_tag_framebuffer);
                out->red_pos = p[0]; out->red_size = p[1];
                out->green_pos = p[2]; out->green_size = p[3];
                out->blue_pos = p[4]; out->blue_size = p[5];
            }
            out->found = 1;
            return 1;
        }

        uint32_t advance = (tag->size + 7) & ~7u;
        ptr += advance;
    }
    return 0;
}

// Tag type 1: the command line GRUB was given for this kernel, as a
// NUL-terminated string immediately after the tag header. Returns 0
// when there is no such tag, which is the normal case for this repo's
// grub.cfg -- so a caller must handle absence rather than an empty
// string. First user: reloc.c's `nokaslr`, the off switch for kernel
// ASLR and the only recovery path if a machine cannot survive being
// relocated.
const char *multiboot_cmdline(void) {
    if (mb_info_addr == 0) return 0;

    uint8_t *base = (uint8_t *)(uintptr_t)mb_info_addr;
    uint32_t total_size = *(uint32_t *)base;
    uint8_t *ptr = base + 8;
    uint8_t *end = base + total_size;

    while (ptr < end) {
        struct mb_tag *tag = (struct mb_tag *)ptr;
        if (tag->type == 0) break;
        if (tag->type == 1) return (const char *)(tag + 1);
        ptr += (tag->size + 7) & ~7u; // tags are 8-byte aligned
    }
    return 0;
}

int multiboot_cmdline_value(const char *key, char *out, uint32_t out_size) {
    const char *cmdline = multiboot_cmdline();
    if (!cmdline || !key || !out || out_size == 0) return 0;
    uint32_t klen = (uint32_t)k_strlen(key);

    for (const char *p = cmdline; (p = k_strstr(p, key)) != 0; p += klen) {
        if (p != cmdline && p[-1] != ' ') continue;
        const char *v = p + klen;
        uint32_t n = 0;
        while (v[n] && v[n] != ' ') n++;
        if (n == 0 || n >= out_size) return 0;  // refused, not truncated
        k_memcpy(out, v, n);
        out[n] = '\0';
        return 1;
    }
    return 0;
}

void multiboot_mmap_foreach(void (*cb)(const struct multiboot_mmap_region *region)) {
    if (mb_info_addr == 0) return;

    uint8_t *base = (uint8_t *)(uintptr_t)mb_info_addr;
    uint32_t total_size = *(uint32_t *)base;
    uint8_t *ptr = base + 8;
    uint8_t *end = base + total_size;

    while (ptr < end) {
        struct mb_tag *tag = (struct mb_tag *)ptr;
        if (tag->type == 0) break;

        if (tag->type == 6) {
            struct mb_tag_mmap *mmap = (struct mb_tag_mmap *)tag;
            uint32_t entry_count = (mmap->size - 16) / mmap->entry_size;
            uint8_t *entry_ptr = (uint8_t *)mmap->entries;
            for (uint32_t i = 0; i < entry_count; i++) {
                struct mb_mmap_entry *e = (struct mb_mmap_entry *)entry_ptr;
                struct multiboot_mmap_region region = { e->base_addr, e->length, e->type };
                cb(&region);
                entry_ptr += mmap->entry_size;
            }
            return; // only one mmap tag exists
        }

        uint32_t advance = (tag->size + 7) & ~7u;
        ptr += advance;
    }
}

void multiboot_print_meminfo(void) {
    if (mb_info_addr == 0) {
        vga_write("multiboot info not available\n");
        return;
    }

    uint8_t *base = (uint8_t *)(uintptr_t)mb_info_addr;
    uint32_t total_size = *(uint32_t *)base;
    uint8_t *ptr = base + 8; // skip total_size + reserved
    uint8_t *end = base + total_size;

    int found_any = 0;

    while (ptr < end) {
        struct mb_tag *tag = (struct mb_tag *)ptr;
        if (tag->type == 0) break; // end tag

        if (tag->type == 4) {
            struct mb_tag_basic_meminfo *m = (struct mb_tag_basic_meminfo *)tag;
            vga_write("Lower memory: ");
            vga_write_dec(m->mem_lower);
            vga_write(" KB\n");
            vga_write("Upper memory: ");
            vga_write_dec(m->mem_upper);
            vga_write(" KB (~");
            vga_write_dec(m->mem_upper / 1024);
            vga_write(" MB)\n");
            found_any = 1;
        } else if (tag->type == 6) {
            struct mb_tag_mmap *mmap = (struct mb_tag_mmap *)tag;
            uint32_t entry_count = (mmap->size - 16) / mmap->entry_size;
            vga_write("Memory map (");
            vga_write_dec(entry_count);
            vga_write(" regions):\n");

            uint8_t *entry_ptr = (uint8_t *)mmap->entries;
            for (uint32_t i = 0; i < entry_count; i++) {
                struct mb_mmap_entry *e = (struct mb_mmap_entry *)entry_ptr;
                vga_write("  ");
                write_hex64(e->base_addr);
                vga_write(" - ");
                write_hex64(e->base_addr + e->length);
                vga_write("  ");
                vga_write(mmap_type_name(e->type));
                vga_putc('\n');
                entry_ptr += mmap->entry_size;
            }
            found_any = 1;
        }

        // tags are 8-byte aligned
        uint32_t advance = (tag->size + 7) & ~7u;
        ptr += advance;
    }

    if (!found_any) {
        vga_write("No memory info tags found in multiboot data\n");
    }
}

struct mb_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char cmdline[];
};

int multiboot_get_module(int index, struct multiboot_module_info *out) {
    out->found = 0;
    if (mb_info_addr == 0) return 0;

    uint8_t *base = (uint8_t *)(uintptr_t)mb_info_addr;
    uint32_t total_size = *(uint32_t *)base;
    uint8_t *ptr = base + 8;
    uint8_t *end = base + total_size;

    int seen = 0;
    while (ptr < end) {
        struct mb_tag *tag = (struct mb_tag *)ptr;
        if (tag->type == 0) break;

        if (tag->type == 3) {
            if (seen == index) {
                struct mb_tag_module *m = (struct mb_tag_module *)tag;
                out->start = m->mod_start;
                out->end = m->mod_end;
                out->found = 1;
                return 1;
            }
            seen++;
        }

        uint32_t advance = (tag->size + 7) & ~7u;
        ptr += advance;
    }
    return 0;
}

// Tags 14 (ACPI 1.0 RSDP) and 15 (ACPI 2.0+ RSDP). Tag 15 wins when
// both are present: it carries the XSDT, and a v1 RSDP beside it
// describes the same tables through 32-bit pointers.
const void *multiboot_acpi_rsdp(uint32_t *out_bytes) {
    if (mb_info_addr == 0) return 0;

    uint8_t *base = (uint8_t *)(uintptr_t)mb_info_addr;
    uint32_t total_size = *(uint32_t *)base;
    uint8_t *ptr = base + 8;
    uint8_t *end = base + total_size;

    const void *old = 0;
    uint32_t old_bytes = 0;

    while (ptr < end) {
        struct mb_tag *tag = (struct mb_tag *)ptr;
        if (tag->type == 0) break;

        if (tag->type == 15) {
            if (out_bytes) *out_bytes = tag->size - 8;
            return (const void *)(tag + 1);
        }
        if (tag->type == 14 && !old) {
            old = (const void *)(tag + 1);
            old_bytes = tag->size - 8;
        }

        ptr += (tag->size + 7) & ~7u;
    }

    if (old && out_bytes) *out_bytes = old_bytes;
    return old;
}
