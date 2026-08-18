// Tests for the user-memory copy helpers (vmm.h) and the two CR4 bits
// that make them mandatory (paging.h).
//
// These build a real address space with real user mappings rather than
// asserting against a live process: a KTEST runs on the kernel context,
// so there is no ring-3 process whose memory it could borrow, and
// constructing one here is what lets the page-boundary and
// bad-mapping cases be tested at all -- both are hard to provoke from a
// program and are exactly where a copy helper goes wrong.
//
// **These do not prove SMAP is enforcing anything**, and cannot: the
// helpers deliberately never touch a user mapping, so they behave
// identically with the bit set or clear. What proves enforcement is a
// deliberate raw dereference, which faults and ends the boot -- see
// docs/decisions.md for the two-line probe.
#include "ktest.h"
#include "vmm.h"
#include "pmm.h"
#include "paging.h"
#include "string.h"

#define TEST_VADDR 0x9000000000ULL // well clear of VMM_USER_BASE's own users

// Builds an address space with `pages` consecutive user pages mapped at
// TEST_VADDR, returning it (0 on failure). The caller frees it with
// vmm_destroy_address_space(), which frees the mapped frames too.
static uint64_t make_space(int pages, uint64_t *frames) {
    uint64_t as = vmm_create_address_space();
    if (!as) return 0;
    for (int i = 0; i < pages; i++) {
        uint64_t f = pmm_alloc_frame();
        if (!f) return 0;
        frames[i] = f;
        if (!vmm_map_user_page(as, TEST_VADDR + (uint64_t)i * 4096, f)) return 0;
    }
    return as;
}

KTEST("uaccess", "a round trip through user memory preserves every byte") {
    uint64_t frames[1];
    uint64_t as = make_space(1, frames);
    KTEST_ASSERT(as != 0);

    char out[64], back[64];
    for (int i = 0; i < 64; i++) out[i] = (char)(i * 3 + 1);

    KTEST_ASSERT(vmm_copy_to_user(as, TEST_VADDR + 100, out, sizeof out));
    k_memset(back, 0, sizeof back);
    KTEST_ASSERT(vmm_copy_from_user(as, back, TEST_VADDR + 100, sizeof back));
    KTEST_ASSERT_EQ(k_memcmp(out, back, sizeof out), 0);

    vmm_destroy_address_space(as);
}

KTEST("uaccess", "a copy spanning a page boundary is correct on both sides") {
    // The case a single-page implementation gets wrong while passing
    // every other test here: two mappings are involved, and the second
    // is a different frame at a different physical address.
    uint64_t frames[2];
    uint64_t as = make_space(2, frames);
    KTEST_ASSERT(as != 0);

    // static, not automatic: 1 KiB of buffers is over the kernel's
    // per-function frame budget (Makefile's -Wframe-larger-than), and a
    // KTEST body is never reentered, so there is nothing to share.
    static char out[512], back[512];
    for (int i = 0; i < 512; i++) out[i] = (char)(i ^ 0x5A);

    // Starts 256 bytes before the boundary, so 256 bytes land in each page.
    uint64_t at = TEST_VADDR + 4096 - 256;
    KTEST_ASSERT(vmm_copy_to_user(as, at, out, sizeof out));
    KTEST_ASSERT(vmm_copy_from_user(as, back, at, sizeof back));
    KTEST_ASSERT_EQ(k_memcmp(out, back, sizeof out), 0);

    // And the bytes really are in two different frames, not one --
    // otherwise the split was never exercised. Read them back through
    // the frames directly.
    const char *p0 = (const char *)(uintptr_t)frames[0];
    const char *p1 = (const char *)(uintptr_t)frames[1];
    KTEST_ASSERT_EQ(k_memcmp(p0 + 4096 - 256, out, 256), 0);
    KTEST_ASSERT_EQ(k_memcmp(p1, out + 256, 256), 0);

    vmm_destroy_address_space(as);
}

KTEST("uaccess", "an unmapped page is refused, in both directions") {
    uint64_t frames[1];
    uint64_t as = make_space(1, frames);
    KTEST_ASSERT(as != 0);

    char buf[16];
    // One page past the only mapping.
    KTEST_ASSERT(!vmm_copy_from_user(as, buf, TEST_VADDR + 4096, sizeof buf));
    KTEST_ASSERT(!vmm_copy_to_user(as, TEST_VADDR + 4096, buf, sizeof buf));

    // And a range that STARTS valid and runs off the end -- the shape a
    // length check alone would miss.
    KTEST_ASSERT(!vmm_copy_from_user(as, buf, TEST_VADDR + 4090, sizeof buf));

    vmm_destroy_address_space(as);
}

KTEST("uaccess", "a kernel address is refused even though it is mapped") {
    // The whole reason these helpers check USER at every level: kernel
    // memory is present in every address space (PML4 entry 0 is shared),
    // so "is it mapped" is not the question.
    uint64_t frames[1];
    uint64_t as = make_space(1, frames);
    KTEST_ASSERT(as != 0);

    char buf[16];
    KTEST_ASSERT(!vmm_copy_from_user(as, buf, 0x100000, sizeof buf));
    KTEST_ASSERT(!vmm_copy_to_user(as, 0x100000, buf, sizeof buf));

    vmm_destroy_address_space(as);
}

KTEST("uaccess", "a wrapping range is refused rather than wrapping") {
    uint64_t frames[1];
    uint64_t as = make_space(1, frames);
    KTEST_ASSERT(as != 0);

    char buf[16];
    KTEST_ASSERT(!vmm_copy_from_user(as, buf, 0xFFFFFFFFFFFFFFF0ULL, 32));

    vmm_destroy_address_space(as);
}

KTEST("uaccess", "a zero-length copy succeeds and touches nothing") {
    // A NULL pointer with a zero length is a legal no-op -- rejecting it
    // would break SYS_GETRANDOM's documented zero-count case.
    uint64_t frames[1];
    uint64_t as = make_space(1, frames);
    KTEST_ASSERT(as != 0);

    char buf[1] = { 0x7E };
    KTEST_ASSERT(vmm_copy_from_user(as, buf, 0, 0));
    KTEST_ASSERT_EQ(buf[0], 0x7E);

    vmm_destroy_address_space(as);
}

KTEST("uaccess", "a user string stops at its NUL and is always terminated") {
    uint64_t frames[1];
    uint64_t as = make_space(1, frames);
    KTEST_ASSERT(as != 0);

    const char *msg = "/bin/ls";
    KTEST_ASSERT(vmm_copy_to_user(as, TEST_VADDR, msg, 8));

    char dst[32];
    k_memset(dst, 0x11, sizeof dst);
    KTEST_ASSERT(vmm_copy_string_from_user(as, dst, TEST_VADDR, sizeof dst));
    KTEST_ASSERT_EQ(k_strcmp(dst, "/bin/ls"), 0);
    // Nothing past the NUL was written -- a bulk copy would have filled
    // the whole buffer.
    KTEST_ASSERT_EQ(dst[8], 0x11);

    vmm_destroy_address_space(as);
}

KTEST("uaccess", "a too-long user string truncates rather than failing") {
    uint64_t frames[1];
    uint64_t as = make_space(1, frames);
    KTEST_ASSERT(as != 0);

    char big[32];
    for (int i = 0; i < 31; i++) big[i] = 'x';
    big[31] = '\0';
    KTEST_ASSERT(vmm_copy_to_user(as, TEST_VADDR, big, sizeof big));

    char dst[8];
    KTEST_ASSERT(vmm_copy_string_from_user(as, dst, TEST_VADDR, sizeof dst));
    KTEST_ASSERT_EQ(k_strlen(dst), 7); // 7 chars + NUL
    KTEST_ASSERT_EQ(dst[7], '\0');

    vmm_destroy_address_space(as);
}

KTEST("uaccess", "a string in unmapped memory is refused, not truncated") {
    uint64_t frames[1];
    uint64_t as = make_space(1, frames);
    KTEST_ASSERT(as != 0);

    char dst[16];
    KTEST_ASSERT(!vmm_copy_string_from_user(as, dst, TEST_VADDR + 4096, sizeof dst));

    vmm_destroy_address_space(as);
}

KTEST("smep_smap", "CR4 agrees with what the enable call reported") {
    // Not "both bits are on" -- QEMU's default qemu64 model supports
    // neither, and demanding them there would make the suite red on the
    // configuration every other test runs under. What must hold is that
    // the kernel's report and the register agree; a mismatch means
    // kernel_main() logged something the CPU did not do.
    int state = paging_smep_smap_state();
    int again = paging_enable_smep_smap(); // idempotent -- setting a set bit
    KTEST_ASSERT_EQ(state, again);
    KTEST_ASSERT_EQ(state, paging_smep_smap_state());
}

KTEST("smep_smap", "a supported bit is actually set, and an unsupported one is not") {
    // The pairing that makes the test above mean something: tie the CR4
    // state to CPUID rather than to itself. Run this under `--cpu max`
    // and it asserts both bits ON; under the default qemu64 it asserts
    // both OFF. Either way it can fail.
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                                : "a"(7), "c"(0));
    int cpu_smep = (int)((ebx >> 7) & 1u);
    int cpu_smap = (int)((ebx >> 20) & 1u);

    int state = paging_smep_smap_state();
    KTEST_ASSERT_EQ(!!(state & PAGING_SMEP_ON), cpu_smep);
    KTEST_ASSERT_EQ(!!(state & PAGING_SMAP_ON), cpu_smap);
}

// --- frame ownership -------------------------------------------------
//
// vmm_destroy_address_space() frees the frames a mapping OWNS and must
// leave a borrowed one alone. The two tests below are each other's
// control: the same walk, the same teardown, one frame owned and one
// borrowed, and only the owned one comes back.
//
// The bug that produced them: every GUI client maps the kernel's glyph
// tables read-only (WIN_FONT_VADDR), and nothing unmapped them on exit,
// so the client's teardown returned pages of kernel .rodata to the
// physical allocator -- measured at two frames on the first GUI app to
// close, then silent, because a frame already free cannot be freed
// twice.

KTEST("vmm", "destroying an address space frees the frames it OWNS") {
    uint64_t before = pmm_free_frames();
    uint64_t as = vmm_create_address_space();
    KTEST_ASSERT(as != 0);
    uint64_t f = pmm_alloc_frame();
    KTEST_ASSERT(f != 0);
    KTEST_ASSERT(vmm_map_user_page(as, TEST_VADDR, f));
    vmm_destroy_address_space(as);
    // The frame went back, so the count is where it started.
    KTEST_ASSERT(pmm_free_frames() == before);
}

KTEST("vmm", "destroying an address space leaves a BORROWED frame alone") {
    uint64_t f = pmm_alloc_frame();
    KTEST_ASSERT(f != 0);

    uint64_t before = pmm_free_frames();
    uint64_t as = vmm_create_address_space();
    KTEST_ASSERT(as != 0);
    // Exactly what win_server.c does for the font, the framebuffer and a
    // window buffer: map a frame somebody else owns.
    KTEST_ASSERT(vmm_map_user_borrowed(as, TEST_VADDR, f, 0, 0, VMM_MT_NORMAL));
    vmm_destroy_address_space(as);

    // Unchanged: the borrowed frame is still allocated to its real
    // owner. If teardown freed it, the count would be one HIGHER -- and
    // pmm would hand out a frame somebody is still using.
    KTEST_ASSERT(pmm_free_frames() == before);

    // Still ours to free, which is the whole claim.
    pmm_free_frame(f);
    KTEST_ASSERT(pmm_free_frames() == before + 1);
}
