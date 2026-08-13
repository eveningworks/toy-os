#ifndef CPU_FEATURES_H
#define CPU_FEATURES_H

#include "cpuinfo.h"

// The (CPUID word, bit) -> name table, plus which CR/EFER bit gates each
// feature where one does. Separate from cpuinfo.h, and deliberately NOT
// reachable through kapi.h, for one concrete reason: it's a ~90-entry
// `static const` array, so every translation unit that includes it
// carries a private copy. Two files want it (/bin/lscpu and the Control
// Panel's System Info applet) and they include it explicitly; the other
// ~40 files that include kapi.h should not each be paying for a table
// they never read.
//
// It lives in a shared header rather than being duplicated because the
// alternative has already been tried in this codebase and is worse:
// userland/lspci.c carries "its own class/subclass -> name table"
// precisely because pci_class_name() is kernel code a ring-3 ELF can't
// link against, and that copy can silently drift from the kernel's.
// A header both sides include has one copy of the DATA even if it has
// two copies of the storage.
//
// Names follow Linux's /proc/cpuinfo spelling wherever one exists
// (`sse4_1`, not `SSE4.1`; `pdpe1gb`, not `1GBPages`), so anything
// learned on Linux transfers, and so the two are diffable.

// `enable` values: which CPU_EN_* bit, if any, decides whether a
// supported feature is actually usable right now. 0 means the question
// doesn't apply -- the feature needs nothing switched on, so supported
// implies available.
//
// **It is a GATE, not the feature's own switch**, and the difference
// matters when reading these entries. One CPU_EN_SSE (CR4.OSFXSR) gates
// a dozen rows here, so that bit being set says the SSE machinery is
// usable -- not that `sse4_1` in particular exists. Only the CPUID bit
// answers "does this exist"; the gate answers "and can I use it".
// Reading it the other way produces a plausible-looking invariant that
// is simply false, which is how the first version of cpuid_test.c's
// cross-check failed on the default qemu64 model.
struct cpu_feature_desc {
    const char *name;
    uint8_t  word;   // enum cpu_feature_word
    uint8_t  bit;
    uint32_t enable; // a CPU_EN_* bit, or 0
};

static const struct cpu_feature_desc CPU_FEATURES[] = {
    // ---- CPUID.01H:EDX ------------------------------------------------
    { "fpu",       CPU_WORD_1_EDX,  0, CPU_EN_FPU },
    { "vme",       CPU_WORD_1_EDX,  1, 0 },
    { "de",        CPU_WORD_1_EDX,  2, 0 },
    { "pse",       CPU_WORD_1_EDX,  3, 0 },
    { "tsc",       CPU_WORD_1_EDX,  4, 0 },
    { "msr",       CPU_WORD_1_EDX,  5, 0 },
    { "pae",       CPU_WORD_1_EDX,  6, CPU_EN_PAE },
    { "mce",       CPU_WORD_1_EDX,  7, 0 },
    { "cx8",       CPU_WORD_1_EDX,  8, 0 },
    { "apic",      CPU_WORD_1_EDX,  9, 0 },
    { "sep",       CPU_WORD_1_EDX, 11, 0 },
    { "mtrr",      CPU_WORD_1_EDX, 12, 0 },
    { "pge",       CPU_WORD_1_EDX, 13, CPU_EN_PGE },
    { "mca",       CPU_WORD_1_EDX, 14, 0 },
    { "cmov",      CPU_WORD_1_EDX, 15, 0 },
    { "pat",       CPU_WORD_1_EDX, 16, 0 },
    { "pse36",     CPU_WORD_1_EDX, 17, 0 },
    { "clflush",   CPU_WORD_1_EDX, 19, 0 },
    { "mmx",       CPU_WORD_1_EDX, 23, 0 },
    { "fxsr",      CPU_WORD_1_EDX, 24, CPU_EN_SSE },
    { "sse",       CPU_WORD_1_EDX, 25, CPU_EN_SSE },
    { "sse2",      CPU_WORD_1_EDX, 26, CPU_EN_SSE },
    { "ss",        CPU_WORD_1_EDX, 27, 0 },
    { "htt",       CPU_WORD_1_EDX, 28, 0 },

    // ---- CPUID.01H:ECX ------------------------------------------------
    { "sse3",      CPU_WORD_1_ECX,  0, CPU_EN_SSE },
    { "pclmulqdq", CPU_WORD_1_ECX,  1, CPU_EN_SSE },
    { "monitor",   CPU_WORD_1_ECX,  3, 0 },
    { "vmx",       CPU_WORD_1_ECX,  5, 0 },
    { "est",       CPU_WORD_1_ECX,  7, 0 },
    { "ssse3",     CPU_WORD_1_ECX,  9, CPU_EN_SSE },
    { "fma",       CPU_WORD_1_ECX, 12, CPU_EN_OSXSAVE },
    { "cx16",      CPU_WORD_1_ECX, 13, 0 },
    { "pcid",      CPU_WORD_1_ECX, 17, 0 },
    { "sse4_1",    CPU_WORD_1_ECX, 19, CPU_EN_SSE },
    { "sse4_2",    CPU_WORD_1_ECX, 20, CPU_EN_SSE },
    { "x2apic",    CPU_WORD_1_ECX, 21, 0 },
    { "movbe",     CPU_WORD_1_ECX, 22, 0 },
    { "popcnt",    CPU_WORD_1_ECX, 23, 0 },
    { "aes",       CPU_WORD_1_ECX, 25, CPU_EN_SSE },
    { "xsave",     CPU_WORD_1_ECX, 26, CPU_EN_OSXSAVE },
    { "osxsave",   CPU_WORD_1_ECX, 27, 0 },
    { "avx",       CPU_WORD_1_ECX, 28, CPU_EN_OSXSAVE },
    { "f16c",      CPU_WORD_1_ECX, 29, CPU_EN_OSXSAVE },
    { "rdrand",    CPU_WORD_1_ECX, 30, 0 },
    { "hypervisor",CPU_WORD_1_ECX, 31, 0 },

    // ---- CPUID.07H:0:EBX ----------------------------------------------
    { "fsgsbase",  CPU_WORD_7_0_EBX,  0, 0 },
    { "sgx",       CPU_WORD_7_0_EBX,  2, 0 },
    { "bmi1",      CPU_WORD_7_0_EBX,  3, 0 },
    { "hle",       CPU_WORD_7_0_EBX,  4, 0 },
    { "avx2",      CPU_WORD_7_0_EBX,  5, CPU_EN_OSXSAVE },
    { "smep",      CPU_WORD_7_0_EBX,  7, CPU_EN_SMEP },
    { "bmi2",      CPU_WORD_7_0_EBX,  8, 0 },
    { "erms",      CPU_WORD_7_0_EBX,  9, 0 },
    { "invpcid",   CPU_WORD_7_0_EBX, 10, 0 },
    { "rtm",       CPU_WORD_7_0_EBX, 11, 0 },
    { "mpx",       CPU_WORD_7_0_EBX, 14, 0 },
    { "avx512f",   CPU_WORD_7_0_EBX, 16, CPU_EN_OSXSAVE },
    { "avx512dq",  CPU_WORD_7_0_EBX, 17, CPU_EN_OSXSAVE },
    { "rdseed",    CPU_WORD_7_0_EBX, 18, 0 },
    { "adx",       CPU_WORD_7_0_EBX, 19, 0 },
    { "smap",      CPU_WORD_7_0_EBX, 20, CPU_EN_SMAP },
    { "clflushopt",CPU_WORD_7_0_EBX, 23, 0 },
    { "clwb",      CPU_WORD_7_0_EBX, 24, 0 },
    { "avx512cd",  CPU_WORD_7_0_EBX, 28, CPU_EN_OSXSAVE },
    { "sha_ni",    CPU_WORD_7_0_EBX, 29, 0 },
    { "avx512bw",  CPU_WORD_7_0_EBX, 30, CPU_EN_OSXSAVE },
    { "avx512vl",  CPU_WORD_7_0_EBX, 31, CPU_EN_OSXSAVE },

    // ---- CPUID.07H:0:ECX ----------------------------------------------
    { "avx512vbmi",CPU_WORD_7_0_ECX,  1, CPU_EN_OSXSAVE },
    { "umip",      CPU_WORD_7_0_ECX,  2, CPU_EN_UMIP },
    { "pku",       CPU_WORD_7_0_ECX,  3, 0 },
    { "ospke",     CPU_WORD_7_0_ECX,  4, 0 },
    { "gfni",      CPU_WORD_7_0_ECX,  8, 0 },
    { "vaes",      CPU_WORD_7_0_ECX,  9, CPU_EN_OSXSAVE },
    { "la57",      CPU_WORD_7_0_ECX, 16, 0 },
    { "rdpid",     CPU_WORD_7_0_ECX, 22, 0 },

    // ---- CPUID.07H:0:EDX ----------------------------------------------
    { "fsrm",      CPU_WORD_7_0_EDX,  4, 0 },
    { "md_clear",  CPU_WORD_7_0_EDX, 10, 0 },
    { "spec_ctrl", CPU_WORD_7_0_EDX, 26, 0 },
    { "stibp",     CPU_WORD_7_0_EDX, 27, 0 },
    { "l1d_flush", CPU_WORD_7_0_EDX, 28, 0 },
    { "arch_capabilities", CPU_WORD_7_0_EDX, 29, 0 },
    { "ssbd",      CPU_WORD_7_0_EDX, 31, 0 },

    // ---- CPUID.80000001H:EDX ------------------------------------------
    { "syscall",   CPU_WORD_EXT1_EDX, 11, 0 },
    { "nx",        CPU_WORD_EXT1_EDX, 20, CPU_EN_NX },
    { "pdpe1gb",   CPU_WORD_EXT1_EDX, 26, 0 },
    { "rdtscp",    CPU_WORD_EXT1_EDX, 27, 0 },
    { "lm",        CPU_WORD_EXT1_EDX, 29, CPU_EN_LONG_MODE },

    // ---- CPUID.80000001H:ECX ------------------------------------------
    { "lahf_lm",   CPU_WORD_EXT1_ECX,  0, 0 },
    { "svm",       CPU_WORD_EXT1_ECX,  2, 0 },
    { "abm",       CPU_WORD_EXT1_ECX,  5, 0 },
    { "sse4a",     CPU_WORD_EXT1_ECX,  6, CPU_EN_SSE },
    { "3dnowprefetch", CPU_WORD_EXT1_ECX, 8, 0 },
};

#define CPU_FEATURE_COUNT ((int)(sizeof(CPU_FEATURES) / sizeof(CPU_FEATURES[0])))

// The CPU_EN_* bits, named, for printing the "what this kernel switched
// on" section. Kept next to the feature table so a new CPU_EN_* bit
// can't be added in cpuinfo.h without an obvious place to name it.
struct cpu_enable_desc {
    const char *name;
    uint32_t bit;
    const char *source; // which register actually holds it
};

static const struct cpu_enable_desc CPU_ENABLES[] = {
    { "paging",       CPU_EN_PAGING,    "CR0.PG" },
    { "write-protect",CPU_EN_WP,        "CR0.WP" },
    { "fpu",          CPU_EN_FPU,       "CR0.EM=0,MP" },
    { "sse",          CPU_EN_SSE,       "CR4.OSFXSR" },
    { "simd-except",  CPU_EN_XMMEXCPT,  "CR4.OSXMMEXCPT" },
    { "pae",          CPU_EN_PAE,       "CR4.PAE" },
    { "global-pages", CPU_EN_PGE,       "CR4.PGE" },
    { "xsave",        CPU_EN_OSXSAVE,   "CR4.OSXSAVE" },
    { "smep",         CPU_EN_SMEP,      "CR4.SMEP" },
    { "smap",         CPU_EN_SMAP,      "CR4.SMAP" },
    { "umip",         CPU_EN_UMIP,      "CR4.UMIP" },
    { "nx",           CPU_EN_NX,        "EFER.NXE" },
    { "long-mode",    CPU_EN_LONG_MODE, "EFER.LMA" },
};

#define CPU_ENABLE_COUNT ((int)(sizeof(CPU_ENABLES) / sizeof(CPU_ENABLES[0])))

#endif
