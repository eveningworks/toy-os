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
#include "vga.h"        // `acpidebug` prints where a reboot cannot erase it
#include "multiboot.h"  // ...and the boot word that turns it on
#include "string.h"
#include "kfmt.h" // klog_printf
#include "knum.h" // k_parse_hex/k_parse_u64, for gpewake=
#include "string.h"

// FADT field offsets, from ACPI 6.4 table 5.9. Named rather than
// commented so a reader can check one against the spec without counting.
#define FADT_SMI_CMD        48
#define FADT_ACPI_ENABLE    52
#define FADT_PM1A_EVT_BLK   56
#define FADT_PM1B_EVT_BLK   60
#define FADT_PM1A_CNT_BLK   64
#define FADT_PM1B_CNT_BLK   68
#define FADT_PM_TMR_BLK     76
#define FADT_GPE0_BLK       80
#define FADT_GPE1_BLK       84
#define FADT_PM1_EVT_LEN    88
#define FADT_PM1_CNT_LEN    89
#define FADT_PM_TMR_LEN     91
#define FADT_GPE0_BLK_LEN   92
#define FADT_GPE1_BLK_LEN   93
#define FADT_FLAGS         112
#define FADT_RESET_REG     116
#define FADT_RESET_VALUE   128
#define FADT_X_DSDT        140
#define FADT_X_PM1A_EVT    148
#define FADT_X_PM1B_EVT    160
#define FADT_X_PM_TMR_BLK  208
#define FADT_X_GPE0_BLK    220
#define FADT_X_GPE1_BLK    232
#define FADT_X_PM1A_CNT    172
#define FADT_X_PM1B_CNT    184
#define FADT_SLEEP_CONTROL 244
#define FADT_SLEEP_STATUS  256
#define FADT_DSDT           40

#define FADT_FLAG_TMR_VAL_EXT   (1u << 8)
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
    fadt_gas(FADT_X_GPE0_BLK, &g);
    s->gpe0_blk = (g.space_id == ACPI_GAS_IO && g.address)
                    ? (uint32_t)g.address : fadt_u32(FADT_GPE0_BLK);
    fadt_gas(FADT_X_GPE1_BLK, &g);
    s->gpe1_blk = (g.space_id == ACPI_GAS_IO && g.address)
                    ? (uint32_t)g.address : fadt_u32(FADT_GPE1_BLK);
    s->gpe0_len = fadt_u8(FADT_GPE0_BLK_LEN);
    s->gpe1_len = fadt_u8(FADT_GPE1_BLK_LEN);
    // The PM timer: a 3.579545 MHz counter the chipset runs whatever the
    // CPU is doing. A length other than 4 means the block is absent.
    fadt_gas(FADT_X_PM_TMR_BLK, &g);
    if (fadt_u8(FADT_PM_TMR_LEN) == 4) {
        s->pm_tmr = (g.space_id == ACPI_GAS_IO && g.address)
                      ? (uint32_t)g.address : fadt_u32(FADT_PM_TMR_BLK);
        s->pm_tmr_bits = (flags & FADT_FLAG_TMR_VAL_EXT) ? 32 : 24;
    }

    if (s->pm1a_cnt && (inw((uint16_t)s->pm1a_cnt) & PM1_CNT_SCI_EN))
        s->flags |= ACPI_F_ENABLED;

    find_s5(s);

    klog_printf("acpi: FADT pm1a_evt=0x%x pm1b_evt=0x%x len=%u "
                "gpe0=0x%x/%u gpe1=0x%x/%u\n",
                s->pm1a_evt, s->pm1b_evt, s->pm1_evt_len,
                s->gpe0_blk, s->gpe0_len, s->gpe1_blk, s->gpe1_len);
    klog_printf("acpi: FADT pm1a=0x%x pm1b=0x%x smi=0x%x pm_tmr=0x%x/%u%s%s\n",
                s->pm1a_cnt, s->pm1b_cnt, s->smi_cmd, s->pm_tmr, s->pm_tmr_bits,
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

// --- the S5 EXPERIMENTS: boot words that change one thing per boot ----
//
// Built to find why the ASUS took two presses to start after toy-os
// powered it off and one after the firmware did; `acpimode=never` found
// it (acpi_poweroff() below). Kept, as `nogpe` is, because the next
// machine is a different machine -- one boot each:
//
//   acpimode=never     the legacy-mode S5 write, with no ACPI-mode fallback
//   acpimode=poweroff  the OLD order: enter ACPI mode, then write S5 --
//                      what needed two presses (acpi_poweroff() below)
//   acpimode=boot      enter ACPI mode at boot, as Linux does
//   gpewake=A,B,..  after masking every GPE, re-arm these (hex or decimal)
//
// The value after `word=`, up to the next space, or NULL when absent.
static const char *boot_value(const char *word, char *buf, size_t cap) {
    const char *cmdline = multiboot_cmdline();
    const char *p = cmdline ? k_strstr(cmdline, word) : 0;
    if (!p || cap == 0) return 0;
    p += k_strlen(word);
    size_t n = 0;
    while (p[n] && p[n] != ' ' && n + 1 < cap) { buf[n] = p[n]; n++; }
    buf[n] = 0;
    return buf;
}

static int acpimode_is(const char *want) {
    char v[16];
    return boot_value("acpimode=", v, sizeof v) && k_strcmp(v, want) == 0;
}

// `gpewake=`: re-arm the named GPEs in a block that was just masked. Each
// is a bit in the block's EN half; a number past the block is ignored.
static void gpe_rearm_named(uint32_t base, uint8_t len, uint32_t first) {
    char list[96];
    if (!base || len < 2 || !boot_value("gpewake=", list, sizeof list)) return;
    uint8_t half = (uint8_t)(len / 2);
    char *p = list;
    while (*p) {
        char *end = p;
        while (*end && *end != ',') end++;
        char save = *end;
        *end = 0;
        uint64_t g;
        int ok = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) ? k_parse_hex(p + 2, &g)
                                                                : k_parse_u64(p, &g);
        if (ok && g >= first && g < first + (uint32_t)half * 8) {
            uint32_t bit = (uint32_t)g - first;
            uint16_t en = (uint16_t)(base + half + bit / 8);
            outb(en, (uint8_t)(inb(en) | (1u << (bit % 8))));
            vga_printf("acpi: GPE 0x%x re-armed for S5 (gpewake=)\n", (uint32_t)g);
            klog_printf("acpi: GPE 0x%x re-armed for S5 (gpewake=)\n", (uint32_t)g);
        }
        *end = save;
        p = save ? end + 1 : end;
    }
}

void acpi_power_boot(void) {
    if (!acpimode_is("boot")) return;
    vga_printf("acpi: entering ACPI mode at boot (acpimode=boot)\n");
    enable_acpi_mode(acpi_state_mut());
}

// `nogpe`: leave the GPE blocks alone. The A/B for "which fix was it?"
// -- clearing PM1_STS and disabling the GPEs both landed for one
// symptom, and a machine that now stops is consistent with either.
// Announced ON THE SCREEN because a machine that reboots cannot show a
// log, and a diagnostic whose effect nobody can confirm is worse than
// none. Same family as `nopat` and `notsc`: an answer in one boot
// instead of a bisect.
static int gpes_left_alone(void) {
    const char *cmdline = multiboot_cmdline();
    if (!cmdline || !k_strstr(cmdline, "nogpe")) return 0;
    vga_printf("acpi: GPE blocks left alone (nogpe)\n");
    klog_write("acpi: GPE blocks left alone (nogpe)\n");
    return 1;
}

// EVERY GENERAL PURPOSE EVENT, DISABLED AND CLEARED, AND LEFT THAT WAY.
// A GPE block is [STS][EN] like PM1's, each half GPEx_BLK_LEN/2 bytes.
//
// THREE VERSIONS OF THIS SHIPPED, and the two cleverer ones were wrong.
// Measured on the machine it was written for, whose GPE0_BLK is 32 bytes
// -- 128 events, among them its lid, its embedded controller and USB:
//
//   disable every GPE and leave it so -- the machine POWERS OFF, and
//     takes two presses of the power button to start again.
//   clear the statuses and restore every enable -- it REBOOTS.
//   clear, read the status back, and rearm only the bits that stayed
//     quiet -- it REBOOTS, and nothing had re-latched to be masked.
//
// The third result is the informative one: the source that wakes this
// machine is NOT ASSERTING when the sleep is prepared, so no measurement
// taken at one instant can find it. It fires during or after the
// transition.
//
// AND MASKING EVERYTHING IS LINUX'S S5 ANSWER ON THAT MACHINE TOO: its
// power button is the fixed PM1 one, not a GPE, and none of its 17
// `_PRW`s wakes from S5 (`tools/aml_walk.py --prw`), so the wake set
// Linux would re-arm here is empty. The two presses it then needs are
// NOT this function's -- see the S5 experiments above and docs/bugs.md.
static void gpe_block_off(uint32_t base, uint8_t len) {
    if (!base || len < 2) return;
    uint8_t half = (uint8_t)(len / 2);
    for (uint8_t i = 0; i < half; i++) outb((uint16_t)(base + half + i), 0x00);
    for (uint8_t i = 0; i < half; i++) outb((uint16_t)(base + i), 0xFF);
}

// Preserves everything in PM1_CNT that is not the sleep request -- a
// blind write would clear SCI_EN, which is a different instruction to
// the chipset than "go to S5". (The wake-status bits are NOT here:
// they are in PM1_STS, cleared above.)
static void pm1_sleep_write(uint16_t port, uint8_t typ) {
    uint16_t v = inw(port);
    v = (uint16_t)(v & ~(PM1_CNT_SLP_TYP_MASK | PM1_CNT_SLP_EN));
    v = (uint16_t)(v | ((uint32_t)typ << PM1_CNT_SLP_TYP_SHIFT));
    // TWO WRITES, as acpi_hw_legacy_sleep() does: the sleep TYPE first
    // and the enable second. One write carrying both is what this used
    // to do and what some chipsets are documented not to accept.
    outw(port, v);
    outw(port, (uint16_t)(v | PM1_CNT_SLP_EN));
}

int acpi_poweroff_known(void) {
    const struct acpi_state *s = acpi_get_state();
    if (!(s->flags & ACPI_F_S5)) return 0;
    return (s->flags & ACPI_F_HW_REDUCED) ? (s->sleep_control.address != 0)
                                          : (s->pm1a_cnt != 0);
}

// `acpidebug`: say what is about to be written, ON THE SCREEN, and wait.
//
// A machine that reboots instead of stopping takes the evidence with it
// -- the klog lines describing the attempt are gone before anyone can
// read them, and a live image has no disk to keep them on. So this
// prints the decision where a camera can reach it and holds for about
// ten seconds. Off unless the boot word is present, because a shutdown
// that pauses is a worse shutdown.
static void debug_pause(const struct acpi_state *s) {
    const char *cmdline = multiboot_cmdline();
    if (!cmdline || !k_strstr(cmdline, "acpidebug")) return;

    vga_printf("\nacpi: about to write S5 type %d to PM1a 0x%x\n",
               (int)s->slp_typ_a, s->pm1a_cnt);
    vga_printf("acpi: ACPI mode %s, smi 0x%x enable 0x%x\n",
               (s->flags & ACPI_F_ENABLED) ? "ON" : "OFF (the firmware's legacy path first)",
               s->smi_cmd, s->acpi_enable);
    vga_printf("acpi: pm1_sts 0x%x  gpe0 0x%x/%u  gpe1 0x%x/%u\n",
               s->pm1a_evt, s->gpe0_blk, s->gpe0_len, s->gpe1_blk, s->gpe1_len);
    vga_printf("acpi: GPE blocks disabled and cleared, and left masked.\n");
    vga_printf("acpi: pausing ~10s so this can be read...\n");
    vga_present();
    // Port-I/O delay, not the PIT: this is reached with interrupts off
    // on some paths, where no tick ever advances.
    for (uint32_t i = 0; i < 10000000; i++) io_wait();
}

// The PM1 path's quiesce and write: every GPE masked and cleared, the
// wake statuses cleared, then the sleep type and enable.
static void pm1_s5(const struct acpi_state *s) {
    if (!gpes_left_alone()) {
        gpe_block_off(s->gpe0_blk, s->gpe0_len);
        gpe_block_off(s->gpe1_blk, s->gpe1_len);
        gpe_rearm_named(s->gpe0_blk, s->gpe0_len, 0);   // GPE0 only: no GPE1 base is parsed
    }
    pm1_clear_status(s);
    // LAST, so that one photograph of a machine about to reboot
    // shows everything that was done to it.
    debug_pause(s);
    klog_printf("acpi: S5 via PM1a 0x%x type %d%s in %s mode, status cleared at 0x%x\n",
                s->pm1a_cnt, (int)s->slp_typ_a, s->pm1b_cnt ? " (and PM1b)" : "",
                (s->flags & ACPI_F_ENABLED) ? "ACPI" : "legacy", s->pm1a_evt);
    pm1_sleep_write((uint16_t)s->pm1a_cnt, s->slp_typ_a);
    if (s->pm1b_cnt) pm1_sleep_write((uint16_t)s->pm1b_cnt, s->slp_typ_b);
}

// A port-I/O delay, `n` of about a microsecond each: no tick advances on
// some of the paths that reach this.
static void s5_grace(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) io_wait();
}

// THE FIRMWARE'S S5 FIRST, WHEN IT LEFT ACPI MODE OFF. A firmware still
// in legacy mode at boot has its own sleep path behind the PM1 write --
// on many chipsets an SMI trap of SLP_EN -- and it is the path its own
// power-button shutdown takes. Entering ACPI mode first takes that away
// and hands the firmware an S5 it expects `_PTS` to have prepared, which
// toy-os cannot run. MEASURED on the ASUS UX305FA, same build, one switch:
// entering ACPI mode first, and starting again took two presses of the
// power button; powering off in legacy mode, and it took one
// (docs/decisions.md). Linux and Windows always enter ACPI mode at boot
// and do run `_PTS`; with no interpreter, the firmware's own path is the
// honest substitute. ACPI mode remains the FALLBACK, for firmware whose
// legacy write does nothing. `acpimode=poweroff` is the old order, for an
// A/B; `acpimode=never` is the legacy write with no fallback.
int acpi_poweroff(void) {
    struct acpi_state *s = acpi_state_mut();
    if (!(s->flags & ACPI_F_S5)) return 0;

    if (s->flags & ACPI_F_HW_REDUCED) {
        if (!gas_usable(&s->sleep_control)) return 0;
        // WAK_STS, the hardware-reduced twin of PM1_STS's, and
        // write-1-to-clear the same way.
        if (gas_usable(&s->sleep_status))
            gas_write8(&s->sleep_status, SLP_STS_WAK);
        debug_pause(s);
        klog_printf("acpi: S5 via SLEEP_CONTROL_REG, type %d\n", (int)s->slp_typ_a);
        gas_write8(&s->sleep_control,
                   (uint8_t)(((s->slp_typ_a & 7) << SLP_CTL_TYP_SHIFT) | SLP_CTL_SLP_EN));
        s5_grace(1000);
        klog_write("acpi: the S5 write did not stop the machine\n");
        return 0;
    }
    if (!s->pm1a_cnt) return 0;

    int legacy_first = !(s->flags & ACPI_F_ENABLED) && !acpimode_is("poweroff");
    if (legacy_first) {
        pm1_s5(s);
        // The firmware's path may talk to its embedded controller before
        // it cuts power: half a second, where the hardware's own write
        // takes microseconds.
        s5_grace(500000);
        if (acpimode_is("never")) {
            klog_write("acpi: the legacy S5 write did not stop the machine (acpimode=never)\n");
            return 0;
        }
        klog_write("acpi: the legacy S5 write did not stop the machine -- entering ACPI mode\n");
    }
    enable_acpi_mode(s);
    pm1_s5(s);

    // The chipset acts on the write asynchronously; a few hundred
    // microseconds of port-I/O delay is the conventional grace period
    // before deciding it did not take.
    s5_grace(1000);
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
