#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

// ACPI tables, READ-ONLY, plus the two things this kernel acts on:
// turning the machine off, and resetting it.
//
// **THIS IS NOT AN ACPI SUBSYSTEM AND MUST NOT BECOME ONE.** There is
// no AML interpreter here and none is wanted (docs/smp-design.md says
// why). What exists is a fixed-layout table walk -- RSDP, RSDT/XSDT,
// FADT, MADT -- and one deliberate exception: a BYTE SCAN of the DSDT
// for the `_S5_` object, because the sleep-type values a poweroff must
// write live in AML and nowhere else. Linux and Windows both evaluate
// `\_S5` with a real interpreter; this decodes the one encoding that
// object is allowed to have and REFUSES anything else, which is the
// difference between "works on QEMU" and "works on VirtualBox and real
// hardware". See docs/decisions.md.
//
// Every table pointer here is a physical address dereferenced through
// boot.asm's identity map of the low 4 GiB. Firmware places ACPI tables
// well below that, and a table above it is REFUSED rather than mapped:
// this runs at boot, before there is anything to map with.

// One table's header. Every ACPI description table starts with it, and
// `length` covers the header plus the body.
struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

// ACPI 2.0's Generic Address Structure: a register named by address
// space rather than assumed to be in I/O space. `space_id` 0 is system
// memory and 1 is system I/O; this kernel handles those two and refuses
// the rest (PCI config space, SMBus, and the embedded controller all
// need a driver that does not exist here).
struct acpi_gas {
    uint8_t  space_id;
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

#define ACPI_GAS_MEMORY 0
#define ACPI_GAS_IO     1

// Where the RSDP came from. Worth reporting rather than assuming: a
// scanned RSDP means the bootloader passed no tag, which is the first
// thing to check when a machine's tables look wrong.
#define ACPI_RSDP_NONE      0
#define ACPI_RSDP_MULTIBOOT 1 // multiboot2 tag 14/15 -- the normal case
#define ACPI_RSDP_SCAN      2 // found by scanning the EBDA and 0xE0000+

// Discovers the RSDP, validates it, walks the RSDT/XSDT, and decodes the
// FADT, the DSDT's `_S5_` object and the MADT. Everything after this is
// a read of what it found; nothing here touches a device.
void acpi_init(void);

// The tables this boot found, in the order the RSDT/XSDT lists them.
int acpi_table_count(void);
const struct acpi_sdt_header *acpi_table_at(int index);

// The first table with this 4-character signature ("FACP", "APIC",
// "HPET"), or NULL. The signature is NOT NUL-terminated in the table,
// so this compares four bytes.
const struct acpi_sdt_header *acpi_find_table(const char *sig);

// What the tables said, decoded. Read by the QUERY_ACPI provider and by
// the power paths; a field that could not be established is zero, and
// the flags say which.
#define ACPI_F_TABLES     (1u << 0) // an RSDP was found and its tables walked
#define ACPI_F_FADT       (1u << 1) // a FADT was found
#define ACPI_F_S5         (1u << 2) // `_S5_` decoded -- poweroff can name a sleep type
#define ACPI_F_HW_REDUCED (1u << 3) // FADT flags bit 20: no PM1 blocks, use SLEEP_CONTROL_REG
#define ACPI_F_RESET      (1u << 4) // FADT flags bit 10: RESET_REG is usable
#define ACPI_F_ENABLED    (1u << 5) // SCI_EN is set, so the machine is in ACPI mode
#define ACPI_F_XSDT       (1u << 6) // the tables were reached through the 64-bit XSDT

struct acpi_state {
    uint32_t flags;        // ACPI_F_*
    uint32_t rsdp_source;  // ACPI_RSDP_*
    uint32_t rsdp_revision; // 0 for ACPI 1.0, 2 for 2.0+
    uint32_t table_count;

    uint64_t rsdt_phys;    // whichever of the two was used
    uint64_t dsdt_phys;

    uint32_t pm1a_cnt;     // I/O ports, 0 when hardware-reduced
    uint32_t pm1b_cnt;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;  // the value written to smi_cmd to enter ACPI mode
    uint8_t  slp_typ_a;    // from `_S5_`; meaningless unless ACPI_F_S5
    uint8_t  slp_typ_b;
    uint8_t  reset_value;

    struct acpi_gas reset_reg;
    struct acpi_gas sleep_control; // ACPI 5.0 hardware-reduced sleep
    struct acpi_gas sleep_status;

    uint64_t lapic_phys;   // MADT's Local APIC address (with the type-5 override applied)
    uint32_t ioapic_count;
    uint32_t cpu_count;
};

const struct acpi_state *acpi_get_state(void);

// One logical processor, from the MADT. `apic_id` is what an
// INIT-SIPI-SIPI would be addressed to; nothing starts one yet.
#define ACPI_CPU_ENABLED       (1u << 0) // the firmware says this one is usable
#define ACPI_CPU_ONLINE_CAPABLE (1u << 1) // ...or could be brought up later
#define ACPI_CPU_X2APIC        (1u << 2) // came from a type-9 entry, not type 0

struct acpi_cpu {
    uint32_t acpi_id;
    uint32_t apic_id;
    uint32_t flags; // ACPI_CPU_*
};

int acpi_cpu_count(void);
const struct acpi_cpu *acpi_cpu_at(int index);

// Turns the machine off through ACPI. Does not return on success.
// Returns 0 when it could not even try -- no FADT, no `_S5_`, or a
// register in an address space this kernel does not drive -- which is
// what makes system_poweroff()'s legacy fallback reachable.
//
// It writes SLP_TYP|SLP_EN into PM1a_CNT (and PM1b_CNT when the FADT
// names one), or into SLEEP_CONTROL_REG on a hardware-reduced platform.
int acpi_poweroff(void);

// Resets the machine through the FADT's RESET_REG. Same contract:
// does not return on success, returns 0 when there is nothing to try,
// which leaves system_reboot()'s 8042 pulse as the fallback.
int acpi_reset(void);

// The query providers. Called from acpi_init(), not from kernel_main().
void acpi_query_init(void);

// Parses a `_S5_` package out of `aml` (a DSDT or SSDT body). Exposed
// for the KTESTs, which build the encodings this has to accept and
// reject without needing a machine that has them. Returns 1 and fills
// both types on success, 0 when the object is absent or encoded in a
// way this does not decode.
int acpi_scan_s5(const uint8_t *aml, uint32_t len, uint8_t *out_a, uint8_t *out_b);

// Sums `len` bytes; an ACPI structure is valid when this is 0.
uint8_t acpi_checksum(const void *p, uint32_t len);

#endif
