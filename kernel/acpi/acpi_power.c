// The FADT, the `_S5_` sleep type, and the two things this kernel does
// with them: power off and reset.
//
// THE TRAP THIS FILE EXISTS TO AVOID: every field below is read at a
// FIXED OFFSET into a table whose length varies by ACPI revision, so a
// read must be guarded by that length or it walks off the end of a
// short FADT into whatever follows it -- and what follows it is another
// table, so the garbage looks like plausible numbers. `fadt_u32()` and
// friends are the guard; do not dereference the struct directly.
#include "acpi.h"
#include "acpi_internal.h"
#include "io.h"
#include "klog.h"
#include "kfmt.h" // klog_printf
#include "string.h"

// FADT field offsets, from ACPI 6.4 table 5.9. Named rather than
// commented so a reader can check one against the spec without counting.
#define FADT_SMI_CMD        48
#define FADT_ACPI_ENABLE    52
#define FADT_PM1A_EVT_BLK   56
#define FADT_PM1B_EVT_BLK   60
#define FADT_PM1A_CNT_BLK   64
#define FADT_PM1B_CNT_BLK   68
#define FADT_PM1_EVT_LEN    88
#define FADT_PM1_CNT_LEN    89
#define FADT_FLAGS         112
#define FADT_RESET_REG     116
#define FADT_RESET_VALUE   128
#define FADT_X_DSDT        140
#define FADT_X_PM1A_EVT    148
#define FADT_X_PM1B_EVT    160
#define FADT_X_PM1A_CNT    172
#define FADT_X_PM1B_CNT    184
#define FADT_SLEEP_CONTROL 244
#define FADT_SLEEP_STATUS  256
#define FADT_DSDT           40

#define FADT_FLAG_RESET_REG_SUP (1u << 10)
#define FADT_FLAG_HW_REDUCED    (1u << 20)

// PM1 control register bits (ACPI 6.4 table 4.13).
#define PM1_CNT_SCI_EN   (1u << 0)
#define PM1_CNT_SLP_EN   (1u << 13)
#define PM1_CNT_SLP_TYP_SHIFT 10
#define PM1_CNT_SLP_TYP_MASK  (7u << PM1_CNT_SLP_TYP_SHIFT)

// The hardware-reduced sleep register's own layout (ACPI 6.4 4.8.3.7),
// which is NOT the PM1 one: the type sits at bit 2 and the enable at
// bit 5, in an 8-bit register.
#define SLP_CTL_TYP_SHIFT 2
#define SLP_CTL_SLP_EN    (1u << 5)
#define SLP_STS_WAK       (1u << 7)   // write-1-to-clear, like PM1_STS's

static const uint8_t *g_fadt;
static uint32_t g_fadt_len;

static uint32_t fadt_have(uint32_t off, uint32_t bytes) {
    return g_fadt && off + bytes <= g_fadt_len;
}

static uint8_t fadt_u8(uint32_t off) {
    return fadt_have(off, 1) ? g_fadt[off] : 0;
}

static uint32_t fadt_u32(uint32_t off) {
    if (!fadt_have(off, 4)) return 0;
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | g_fadt[off + i];
    return v;
}

static uint64_t fadt_u64(uint32_t off) {
    if (!fadt_have(off, 8)) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | g_fadt[off + i];
    return v;
}

// A GAS is 12 bytes at a 4-byte-aligned offset inside a packed table,
// so its 64-bit address is copied out rather than loaded in place.
static void fadt_gas(uint32_t off, struct acpi_gas *out) {
    k_memset(out, 0, sizeof *out);
    if (!fadt_have(off, 12)) return;
    out->space_id    = g_fadt[off];
    out->bit_width   = g_fadt[off + 1];
    out->bit_offset  = g_fadt[off + 2];
    out->access_size = g_fadt[off + 3];
    for (int i = 7; i >= 0; i--) out->address = (out->address << 8) | g_fadt[off + 4 + i];
}

// --- the `_S5_` scan --------------------------------------------------
//
// The one piece of AML this kernel reads. `_S5_` is a Name holding a
// Package of small integers, and that is the whole grammar accepted
// here: NameOp, optionally behind a root/parent prefix, then PackageOp,
// a PkgLength, an element count, then the two sleep types.
//
// It REFUSES anything else rather than guessing (this repo's parser
// rule) -- a wrong SLP_TYP is a write of a real sleep request to a real
// register, and "reboots instead of shutting down" is the polite
// version of what that does.
static int aml_small_int(const uint8_t *p, uint32_t left, uint8_t *out, uint32_t *used) {
    if (left < 1) return 0;
    switch (p[0]) {
        case 0x00: *out = 0; *used = 1; return 1; // ZeroOp
        case 0x01: *out = 1; *used = 1; return 1; // OneOp
        case 0x0A:                                 // BytePrefix
            if (left < 2) return 0;
            *out = p[1]; *used = 2; return 1;
        default: return 0;
    }
}

int acpi_scan_s5(const uint8_t *aml, uint32_t len, uint8_t *out_a, uint8_t *out_b) {
    if (!aml || len < 8) return 0;

    for (uint32_t i = 0; i + 4 < len; i++) {
        if (aml[i] != '_' || aml[i + 1] != 'S' ||
            aml[i + 2] != '5' || aml[i + 3] != '_') continue;

        // The name must be introduced by NameOp (0x08), directly or
        // behind a root (`\`, 0x5C) or parent (`^`, 0x5E) prefix.
        // Without this check the same four bytes appearing inside a
        // string or a method body would be decoded as the object.
        if (i >= 1 && aml[i - 1] == 0x08) {
            /* NameOp _S5_ */
        } else if (i >= 2 && aml[i - 2] == 0x08 &&
                   (aml[i - 1] == 0x5C || aml[i - 1] == 0x5E)) {
            /* NameOp \_S5_ */
        } else {
            continue;
        }

        uint32_t p = i + 4;
        if (p >= len || aml[p] != 0x12) continue; // PackageOp
        p++;
        if (p >= len) continue;

        // PkgLength: the top two bits say how many MORE bytes follow.
        p += 1u + ((aml[p] >> 6) & 3u);
        if (p >= len) continue;
        uint8_t elements = aml[p++];
        if (elements < 2) continue;

        uint32_t used = 0;
        uint8_t a = 0, b = 0;
        if (!aml_small_int(aml + p, len - p, &a, &used)) continue;
        p += used;
        if (!aml_small_int(aml + p, len - p, &b, &used)) continue;

        *out_a = a & 7;
        *out_b = b & 7;
        return 1;
    }
    return 0;
}

// The DSDT first, then every SSDT: firmware is allowed to define `_S5_`
// in either, and some does.
static void find_s5(struct acpi_state *s) {
    uint64_t dsdt = fadt_u64(FADT_X_DSDT);
    if (!acpi_phys_readable(dsdt, sizeof(struct acpi_sdt_header)))
        dsdt = fadt_u32(FADT_DSDT);
    if (acpi_phys_readable(dsdt, sizeof(struct acpi_sdt_header))) {
        const struct acpi_sdt_header *h = acpi_phys(dsdt);
        if (h->length > sizeof *h && acpi_phys_readable(dsdt, h->length)) {
            s->dsdt_phys = dsdt;
            if (acpi_scan_s5((const uint8_t *)h + sizeof *h,
                             h->length - (uint32_t)sizeof *h,
                             &s->slp_typ_a, &s->slp_typ_b)) {
                s->flags |= ACPI_F_S5;
                return;
            }
        }
    }

    for (int i = 0; i < acpi_table_count(); i++) {
        const struct acpi_sdt_header *h = acpi_table_at(i);
        if (k_memcmp(h->signature, "SSDT", 4) != 0) continue;
        if (h->length <= sizeof *h) continue;
        if (acpi_scan_s5((const uint8_t *)h + sizeof *h,
                         h->length - (uint32_t)sizeof *h,
                         &s->slp_typ_a, &s->slp_typ_b)) {
            s->flags |= ACPI_F_S5;
            return;
        }
    }
}

void acpi_fadt_init(void) {
    struct acpi_state *s = acpi_state_mut();

    const struct acpi_sdt_header *fadt = 0;
    for (int i = 0; i < acpi_table_count(); i++) {
        // "FACP" is the FADT's signature. The four letters do not spell
        // the table's name and never have.
        if (k_memcmp(acpi_table_at(i)->signature, "FACP", 4) == 0) {
            fadt = acpi_table_at(i);
            break;
        }
    }
    if (!fadt) {
        klog_write("acpi: no FADT -- poweroff and reset stay on the legacy paths\n");
        return;
    }

    g_fadt = (const uint8_t *)fadt;
    g_fadt_len = fadt->length;
    s->flags |= ACPI_F_FADT;

    uint32_t flags = fadt_u32(FADT_FLAGS);
    if (flags & FADT_FLAG_HW_REDUCED) s->flags |= ACPI_F_HW_REDUCED;
    if (flags & FADT_FLAG_RESET_REG_SUP) s->flags |= ACPI_F_RESET;

    s->smi_cmd     = fadt_u32(FADT_SMI_CMD);
    s->acpi_enable = fadt_u8(FADT_ACPI_ENABLE);
    s->reset_value = fadt_u8(FADT_RESET_VALUE);
    fadt_gas(FADT_RESET_REG, &s->reset_reg);
    fadt_gas(FADT_SLEEP_CONTROL, &s->sleep_control);
    fadt_gas(FADT_SLEEP_STATUS, &s->sleep_status);

    // The extended (GAS) forms win when present, as the spec requires --
    // the 32-bit fields exist for pre-2.0 tables and firmware is allowed
    // to zero them once it has filled the X_ versions.
    struct acpi_gas g;
    fadt_gas(FADT_X_PM1A_CNT, &g);
    s->pm1a_cnt = (g.space_id == ACPI_GAS_IO && g.address)
                    ? (uint32_t)g.address : fadt_u32(FADT_PM1A_CNT_BLK);
    fadt_gas(FADT_X_PM1B_CNT, &g);
    s->pm1b_cnt = (g.space_id == ACPI_GAS_IO && g.address)
                    ? (uint32_t)g.address : fadt_u32(FADT_PM1B_CNT_BLK);
    fadt_gas(FADT_X_PM1A_EVT, &g);
    s->pm1a_evt = (g.space_id == ACPI_GAS_IO && g.address)
                    ? (uint32_t)g.address : fadt_u32(FADT_PM1A_EVT_BLK);
    fadt_gas(FADT_X_PM1B_EVT, &g);
    s->pm1b_evt = (g.space_id == ACPI_GAS_IO && g.address)
                    ? (uint32_t)g.address : fadt_u32(FADT_PM1B_EVT_BLK);
    s->pm1_evt_len = fadt_u8(FADT_PM1_EVT_LEN);

    if (s->pm1a_cnt && (inw((uint16_t)s->pm1a_cnt) & PM1_CNT_SCI_EN))
        s->flags |= ACPI_F_ENABLED;

    find_s5(s);

    klog_printf("acpi: FADT pm1a_evt=0x%x pm1b_evt=0x%x len=%u\n",
                s->pm1a_evt, s->pm1b_evt, s->pm1_evt_len);
    klog_printf("acpi: FADT pm1a=0x%x pm1b=0x%x smi=0x%x%s%s\n",
                s->pm1a_cnt, s->pm1b_cnt, s->smi_cmd,
                (s->flags & ACPI_F_HW_REDUCED) ? " hardware-reduced" : "",
                (s->flags & ACPI_F_ENABLED) ? " (ACPI mode already on)" : "");
    if (s->flags & ACPI_F_S5)
        klog_printf("acpi: _S5_ sleep types a=%d b=%d\n",
                    (int)s->slp_typ_a, (int)s->slp_typ_b);
    else
        klog_write("acpi: no usable _S5_ -- ACPI poweroff is unavailable\n");
}

// --- acting on it -----------------------------------------------------

// Enters ACPI mode if the firmware has not already. Silent no-op when
// there is no SMI command port, which is the normal case on a machine
// whose firmware handed control over with SCI_EN already set (and on
// every hardware-reduced platform, where the transition does not exist).
static void enable_acpi_mode(struct acpi_state *s) {
    if (s->flags & (ACPI_F_ENABLED | ACPI_F_HW_REDUCED)) return;
    if (!s->smi_cmd || !s->acpi_enable || !s->pm1a_cnt) return;

    klog_write("acpi: entering ACPI mode via the SMI command port\n");
    outb((uint16_t)s->smi_cmd, s->acpi_enable);

    // The handover is an SMI, so the delay is the firmware's and there
    // is no interrupt to wait on. A bounded spin; giving up here still
    // leaves the write below worth attempting.
    for (int i = 0; i < 100000; i++) {
        if (inw((uint16_t)s->pm1a_cnt) & PM1_CNT_SCI_EN) {
            s->flags |= ACPI_F_ENABLED;
            return;
        }
        io_wait();
    }
    klog_write("acpi: SCI_EN never came up -- trying the sleep write anyway\n");
}

static void gas_write8(const struct acpi_gas *g, uint8_t v) {
    if (g->space_id == ACPI_GAS_IO) {
        outb((uint16_t)g->address, v);
    } else if (g->space_id == ACPI_GAS_MEMORY) {
        *(volatile uint8_t *)(uintptr_t)g->address = v;
    }
}

static int gas_usable(const struct acpi_gas *g) {
    if (!g->address) return 0;
    if (g->space_id == ACPI_GAS_IO) return g->address <= 0xFFFF;
    if (g->space_id == ACPI_GAS_MEMORY) return acpi_phys_readable(g->address, 1);
    return 0; // PCI config space, SMBus, the EC -- no driver for those here
}

// EVERY PENDING WAKE EVENT, CLEARED. PM1_STS is the FIRST HALF of the
// PM1 event block and its bits are write-1-to-clear, so 0xFFFF clears
// whatever is set -- the power-button press that asked for the shutdown
// among them.
//
// Skipping this does not fail: the machine enters S5 with a wake
// already pending and comes straight back up, which reads as "it
// restarts instead of shutting down, forever". Linux clears WAK_STS in
// acpi_hw_legacy_sleep() for the same reason, and the ACPI spec
// requires it.
static void pm1_clear_status(const struct acpi_state *s) {
    // The block is [STS][EN], each half PM1_EVT_LEN/2 bytes -- a length
    // the firmware states and which is 4 on essentially everything.
    uint8_t half = s->pm1_evt_len ? (uint8_t)(s->pm1_evt_len / 2) : 2;
    if (half < 2) return;
    if (s->pm1a_evt) outw((uint16_t)s->pm1a_evt, 0xFFFF);
    if (s->pm1b_evt) outw((uint16_t)s->pm1b_evt, 0xFFFF);
}

// Preserves everything in PM1_CNT that is not the sleep request -- a
// blind write would clear SCI_EN, which is a different instruction to
// the chipset than "go to S5". (The wake-status bits are NOT here:
// they are in PM1_STS, cleared above.)
static void pm1_sleep_write(uint16_t port, uint8_t typ) {
    uint16_t v = inw(port);
    v = (uint16_t)(v & ~(PM1_CNT_SLP_TYP_MASK | PM1_CNT_SLP_EN));
    v = (uint16_t)(v | ((uint32_t)typ << PM1_CNT_SLP_TYP_SHIFT) | PM1_CNT_SLP_EN);
    outw(port, v);
}

int acpi_poweroff_known(void) {
    const struct acpi_state *s = acpi_get_state();
    if (!(s->flags & ACPI_F_S5)) return 0;
    return (s->flags & ACPI_F_HW_REDUCED) ? (s->sleep_control.address != 0)
                                          : (s->pm1a_cnt != 0);
}

int acpi_poweroff(void) {
    struct acpi_state *s = acpi_state_mut();
    if (!(s->flags & ACPI_F_S5)) return 0;

    enable_acpi_mode(s);

    if (s->flags & ACPI_F_HW_REDUCED) {
        if (!gas_usable(&s->sleep_control)) return 0;
        // WAK_STS, the hardware-reduced twin of PM1_STS's, and
        // write-1-to-clear the same way.
        if (gas_usable(&s->sleep_status))
            gas_write8(&s->sleep_status, SLP_STS_WAK);
        klog_printf("acpi: S5 via SLEEP_CONTROL_REG, type %d\n", (int)s->slp_typ_a);
        gas_write8(&s->sleep_control,
                   (uint8_t)(((s->slp_typ_a & 7) << SLP_CTL_TYP_SHIFT) | SLP_CTL_SLP_EN));
    } else {
        if (!s->pm1a_cnt) return 0;
        pm1_clear_status(s);
        klog_printf("acpi: S5 via PM1a 0x%x type %d%s, status cleared at 0x%x\n",
                    s->pm1a_cnt, (int)s->slp_typ_a,
                    s->pm1b_cnt ? " (and PM1b)" : "", s->pm1a_evt);
        pm1_sleep_write((uint16_t)s->pm1a_cnt, s->slp_typ_a);
        if (s->pm1b_cnt) pm1_sleep_write((uint16_t)s->pm1b_cnt, s->slp_typ_b);
    }

    // The chipset acts on the write asynchronously; a few hundred
    // microseconds of port-I/O delay is the conventional grace period
    // before deciding it did not take.
    for (int i = 0; i < 1000; i++) io_wait();
    klog_write("acpi: the S5 write did not stop the machine\n");
    return 0;
}

int acpi_reset(void) {
    struct acpi_state *s = acpi_state_mut();
    if (!(s->flags & ACPI_F_RESET)) return 0;
    if (!gas_usable(&s->reset_reg)) return 0;

    klog_printf("acpi: reset via %s 0x%x value 0x%x\n",
                s->reset_reg.space_id == ACPI_GAS_IO ? "port" : "memory",
                (uint32_t)s->reset_reg.address, (uint32_t)s->reset_value);
    gas_write8(&s->reset_reg, s->reset_value);

    for (int i = 0; i < 1000; i++) io_wait();
    klog_write("acpi: the reset write did not restart the machine\n");
    return 0;
}
