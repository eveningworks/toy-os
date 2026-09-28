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

static const struct kdb_transport script_io = { .getc = script_getc, .putc = script_putc };

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
    script_add("qfThreadInfo", 1);
    script_add("qThreadExtraInfo,1", 1);
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
    // The breakpoint's stop reply names the thread that hit it.
    const char *stop = k_strstr(g_log, "$T05thread:");
    KTEST_ASSERT(stop != 0);

    // The `g` reply opens with 16 GPRs (256 hex digits); rip follows,
    // little-endian -- and must be the breakpoint, not one past the int3.
    char rip[17];
    for (int i = 0; i < 8; i++) {
        uint8_t byte = (uint8_t)(at >> (i * 8));
        k_snprintf(rip + i * 2, 3, "%02x", byte);
    }
    const char *g = stop ? k_strstr(stop + 1, "$") : 0;
    KTEST_ASSERT(g != 0);
    KTEST_ASSERT(k_strncmp(g + 1 + 256, rip, 16) == 0);
    KTEST_ASSERT_EQ(*(volatile uint8_t *)(uintptr_t)at, before);   // lifted for good

    // Threads: the kernel context first (1000 = 0x3e8), and pid 1 by name
    // -- "init, " hex-encoded is 696e69742c20.
    KTEST_ASSERT(k_strstr(g_log, "$m3e8,1") != 0);
    KTEST_ASSERT(k_strstr(g_log, "$696e69742c20") != 0);
}

// --- the network transport's pieces --------------------------------------

static void to_hex(const uint8_t *b, int n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2] = hex[b[i] >> 4];
        out[i * 2 + 1] = hex[b[i] & 15];
    }
    out[n * 2] = 0;
}

KTEST("kdebug", "HMAC-SHA256 matches RFC 4231 test case 2") {
    static const char data[] = "what do ya want for nothing?";
    uint8_t mac[32];
    char hex[65];
    kdb_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)data, 10,
                    (const uint8_t *)data + 10, (int)sizeof data - 11, mac);   // split: two pieces
    to_hex(mac, 32, hex);
    KTEST_ASSERT(k_strcmp(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0);
}

KTEST("kdebug", "kdebug=net parses, and refuses rather than guesses") {
    struct kdb_net_cfg c;
    KTEST_ASSERT(kdb_net_parse("net,ip=10.0.2.15,key=00112233445566778899aabbccddeeff,wait", &c));
    KTEST_ASSERT(c.ip == 0x0A00020F && c.port == 50000 && c.klen == 16 && c.wait);
    KTEST_ASSERT(c.nic_bus == -1);
    KTEST_ASSERT(kdb_net_parse("net,ip=1.2.3.4,port=6000,nic=00:04.0,key=00112233445566778899aabbccddeeff", &c));
    KTEST_ASSERT(c.port == 6000 && c.nic_bus == 0 && c.nic_dev == 4 && c.nic_fn == 0);
    // a short key, no key, no address, a bad address, an unknown word
    KTEST_ASSERT(!kdb_net_parse("net,ip=10.0.2.15,key=0011", &c));
    KTEST_ASSERT(!kdb_net_parse("net,ip=10.0.2.15", &c));
    KTEST_ASSERT(!kdb_net_parse("net,key=00112233445566778899aabbccddeeff", &c));
    KTEST_ASSERT(!kdb_net_parse("net,ip=10.0.2.256,key=00112233445566778899aabbccddeeff", &c));
    KTEST_ASSERT(!kdb_net_parse("net,ip=10.0.2.15,key=00112233445566778899aabbccddeeff,fast", &c));
}

// A host's hello under `key`, as kdebug_bridge.py builds it.
static int hello_q(const uint8_t *key, int klen, uint8_t fill, uint8_t *q) {
    uint8_t mac[32];
    k_memcpy(q, "TKDQ", 4);
    k_memset(q + 4, fill, KDB_NONCE);
    kdb_hmac_sha256(key, klen, q, 4 + KDB_NONCE, 0, 0, mac);
    k_memcpy(q + 4 + KDB_NONCE, mac, 16);
    return KDB_HELLO_Q;
}

KTEST("kdebug", "a datagram is refused when forged, damaged, replayed or from another session") {
    if (kdb.armed) KTEST_SKIP("the stub is armed for a real debugger on this boot");
    static uint8_t d[64], old[64], q[KDB_HELLO_Q], r[KDB_HELLO_R];
    static const uint8_t key[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    struct kdb_net_cfg c;
    KTEST_ASSERT(kdb_net_parse("net,ip=10.0.2.15,key=000102030405060708090a0b0c0d0e0f", &c));
    kdb_net_configure(&c);
    const uint8_t *payload;

    // Nothing is accepted, or sealed, before a hello.
    KTEST_ASSERT_EQ(kdb_net_seal("TKDH", 1, (const uint8_t *)"x", 1, d, sizeof d), -1);

    // A hello under the wrong key gets no session.
    static const uint8_t wrong[16] = { 0xff, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    hello_q(wrong, 16, 0x11, q);
    KTEST_ASSERT_EQ(kdb_net_hello(q, KDB_HELLO_Q, r, sizeof r), -1);

    // The right one: the reply echoes the host's nonce and is keyed.
    hello_q(key, 16, 0x11, q);
    KTEST_ASSERT_EQ(kdb_net_hello(q, KDB_HELLO_Q, r, sizeof r), KDB_HELLO_R);
    uint8_t mac[32];
    kdb_hmac_sha256(key, 16, r, 4 + 2 * KDB_NONCE, 0, 0, mac);
    KTEST_ASSERT(k_memcmp(r, "TKDN", 4) == 0 && k_memcmp(r + 4, q + 4, KDB_NONCE) == 0);
    KTEST_ASSERT(k_memcmp(mac, r + 4 + 2 * KDB_NONCE, 16) == 0);
    uint8_t tn1[KDB_NONCE];
    k_memcpy(tn1, r + 4 + KDB_NONCE, KDB_NONCE);

    int n = kdb_net_seal("TKDH", 7, (const uint8_t *)"$?#3f", 5, d, sizeof d);
    KTEST_ASSERT_EQ(n, KDB_NET_HDR + 5);
    KTEST_ASSERT_EQ(kdb_net_open(d, n, &payload), 5);
    KTEST_ASSERT(k_strncmp((const char *)payload, "$?#3f", 5) == 0);
    KTEST_ASSERT_EQ(kdb_net_open(d, n, &payload), -1);          // the same one again

    n = kdb_net_seal("TKDH", 8, (const uint8_t *)"$g#67", 5, d, sizeof d);
    d[n - 1] ^= 1;                                               // one bit of payload
    KTEST_ASSERT_EQ(kdb_net_open(d, n, &payload), -1);
    d[n - 1] ^= 1;
    KTEST_ASSERT_EQ(kdb_net_open(d, n, &payload), 5);           // intact, it is accepted

    n = kdb_net_seal("TKDT", 9, (const uint8_t *)"x", 1, d, sizeof d);
    KTEST_ASSERT_EQ(kdb_net_open(d, n, &payload), -1);           // the target's own, reflected

    // ANOTHER SESSION -- a reconnect, or a reboot -- where the counters
    // start again: a datagram recorded in the first fails, even one with
    // a sequence number far above anything accepted since.
    int on = kdb_net_seal("TKDH", 1000, (const uint8_t *)"$c#63", 5, old, sizeof old);
    KTEST_ASSERT_EQ(kdb_net_hello(q, KDB_HELLO_Q, r, sizeof r), KDB_HELLO_R);   // the SAME hello, replayed
    KTEST_ASSERT(k_memcmp(r + 4 + KDB_NONCE, tn1, KDB_NONCE) != 0);            // a fresh target nonce
    KTEST_ASSERT_EQ(kdb_net_open(old, on, &payload), -1);
    n = kdb_net_seal("TKDH", 1, (const uint8_t *)"$?#3f", 5, d, sizeof d);
    KTEST_ASSERT_EQ(kdb_net_open(d, n, &payload), 5);            // the new session's own

    k_memset(&c, 0, sizeof c);
    kdb_net_configure(&c);
}
