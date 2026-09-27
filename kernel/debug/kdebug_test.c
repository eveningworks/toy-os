// KTESTs for the kernel debugger. The breakpoint test is end to end: a
// real int3 patched into kernel text, the real trap path, the real
// protocol -- with a SCRIPTED transport playing GDB, so it needs no
// serial port and no host. tools/kdebug_test.py drives a real GDB
// session over a real port.
#include "ktest.h"
#include "kdebug.h"
#include "kdebug_internal.h"
#include "kfmt.h"
#include "string.h"

KTEST("kdebug", "packet checksum and hex digits") {
    KTEST_ASSERT_EQ(kdb_checksum("OK", 2), 0x9a);
    KTEST_ASSERT_EQ(kdb_checksum("", 0), 0);
    KTEST_ASSERT_EQ(kdb_hexval('0'), 0);
    KTEST_ASSERT_EQ(kdb_hexval('f'), 15);
    KTEST_ASSERT_EQ(kdb_hexval('A'), 10);
    KTEST_ASSERT_EQ(kdb_hexval('g'), -1);
}

KTEST("kdebug", "memory access refuses what nothing maps") {
    uint8_t b = 0x5a;
    // Non-canonical: no page table can map it, and touching it is a #GP.
    KTEST_ASSERT_EQ((int)kdb_arch_mem_read(0x0000800000000000ULL, &b, 1), 0);
    KTEST_ASSERT_EQ((int)kdb_arch_mem_write(0x0000800000000000ULL, &b, 1), 0);
    KTEST_ASSERT_EQ(b, 0x5a);

    // A stack variable round-trips at every width the MMIO path uses.
    uint64_t v = 0x1122334455667788ULL, got = 0;
    KTEST_ASSERT_EQ((int)kdb_arch_mem_read((uint64_t)(uintptr_t)&v, &got, 8), 8);
    KTEST_ASSERT(got == v);
    uint32_t w = 0xdeadbeef;
    KTEST_ASSERT_EQ((int)kdb_arch_mem_write((uint64_t)(uintptr_t)&v, &w, 4), 4);
    KTEST_ASSERT(v == 0x11223344deadbeefULL);
}

// --- a scripted debugger -----------------------------------------------

static char g_script[512];
static int g_spos;
static char g_log[2048];
static int g_lpos;

// Only answers while stopped, so a timer tick's break-in poll never eats
// the script. Running dry sends a detach, which cannot hang the test.
static int script_getc(void) {
    static const char detach[] = "$D#44+";
    if (!kdb.active) return -1;
    if (g_script[g_spos]) return (uint8_t)g_script[g_spos++];
    static int d;
    char c = detach[d];
    d = (d + 1) % (int)(sizeof detach - 1);
    return (uint8_t)c;
}

static void script_putc(char c) {
    if (g_lpos < (int)sizeof g_log - 1) g_log[g_lpos++] = c;
    g_log[g_lpos] = 0;
}

static const struct kdb_transport script_io = { script_getc, script_putc };

// "$data#xx+": a packet, then the ack for the stub's reply to it.
static void script_add(const char *data, int ack) {
    int n = (int)k_strlen(data);
    k_snprintf(g_script + k_strlen(g_script), sizeof g_script - k_strlen(g_script),
               "$%s#%02x%s", data, kdb_checksum(data, n), ack ? "+" : "");
}

static volatile int g_sink;

static int __attribute__((noinline)) kdb_test_target(int x) {
    g_sink = x;   // a side effect, so the call is not folded away
    return x * 3 + 1;
}

KTEST("kdebug", "a software breakpoint stops, reports the PC and resumes") {
    if (kdb.armed) KTEST_SKIP("the stub is armed for a real debugger on this boot");

    uint64_t at = (uint64_t)(uintptr_t)kdb_test_target;
    uint8_t before = *(volatile uint8_t *)(uintptr_t)at;
    char bp[64];
    k_snprintf(bp, sizeof bp, "Z0,%lx,1", at);

    g_script[0] = 0;
    g_spos = g_lpos = 0;
    g_log[0] = 0;
    // Stop 1 (the compiled-in int3 below): set the breakpoint, go.
    script_add(bp, 1);
    script_add("c", 0);
    // Stop 2 (the breakpoint): ack the stop reply, read registers,
    // remove it, go.
    k_strlcpy(g_script + k_strlen(g_script), "+", sizeof g_script - k_strlen(g_script));
    script_add("g", 1);
    bp[0] = 'z';
    script_add(bp, 1);
    script_add("c", 0);

    uint32_t stops = kdb.stops;
    kdb.io = &script_io;
    kdb.armed = 1;
    kdb.pending_sig = KDB_SIGTRAP;
    kdb_arch_breakpoint();
    int r = kdb_test_target(5);
    kdb.armed = 0;
    kdb.connected = 0;
    kdb.io = 0;
    kdb_bp_clear_all();

    KTEST_ASSERT_EQ(r, 16);                    // the patched instruction ran as itself
    KTEST_ASSERT_EQ((int)(kdb.stops - stops), 2);
    KTEST_ASSERT(k_strstr(g_log, "$S05#b8") != 0);   // the breakpoint's stop reply

    // The `g` reply opens with 16 GPRs (256 hex digits); rip follows,
    // little-endian -- and must be the breakpoint, not one past the int3.
    char rip[17];
    for (int i = 0; i < 8; i++) {
        uint8_t byte = (uint8_t)(at >> (i * 8));
        k_snprintf(rip + i * 2, 3, "%02x", byte);
    }
    const char *g = k_strstr(g_log, "$S05#b8");
    g = g ? k_strstr(g + 7, "$") : 0;
    KTEST_ASSERT(g != 0);
    KTEST_ASSERT(k_strncmp(g + 1 + 256, rip, 16) == 0);
    KTEST_ASSERT_EQ(*(volatile uint8_t *)(uintptr_t)at, before);   // lifted for good
}
