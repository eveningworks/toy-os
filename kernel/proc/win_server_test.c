// Tests for cross-process window-buffer sharing -- Milestone 41's
// stage 1 (docs/wm-ring3-design.md).
//
// WHY THESE ARE KTESTS AND NOT A GUI TEST
// ---------------------------------------
// The property that matters here is invisible from userland and
// invisible on screen: after a window is destroyed, the compositor's
// MAPPING of its pixels must be gone. Nothing a client or a test tool
// can observe distinguishes "the mapping was revoked" from "the
// mapping still resolves and the frames happen to be untouched so far"
// -- the difference only shows up later, as another allocation's data
// appearing inside a window, which reads as a compositing bug.
//
// Asking the page tables directly is the only honest check, and that
// means running inside the kernel.
//
// HOW A TEST GETS A WINDOW WITHOUT A PROCESS
// ------------------------------------------
// `win_server_create_raw()` (see win_server.h) makes a window in an
// address space the test built with vmm_create_address_space(). No
// process, no protocol, no desktop -- but an ORDINARY window as far as
// every path under test is concerned, subject to the same ownership
// checks as any other.
//
// POSITIVE CONTROL, re-run when the poison contract landed: change
// destroy_window()'s comp_poison() back to comp_unmap() and rebuild.
// Exactly two checks go red -- "destroying a window poisons the
// compositor's mapping" and "a client dying revokes it too" -- and they
// fail on vmm_validate_user_range() rather than on the bookkeeping flag,
// which is the pair worth having. The other five stay green, which is
// itself informative: destroy, resize and unregister each have their OWN
// revocation call, so breaking one does not implicate the others.
// Do this again before trusting a clean run after any change here.
//
// WHAT THESE CANNOT PROVE, stated rather than implied: anything about
// the TLB. Every check here reaches a frame by WALKING the page tables
// from a pml4 the test built, so a PTE that is right while a live
// address space still caches the old translation reads as correct.
// comp_map()'s comp_unpoison() call exists for exactly that hazard --
// mapping over a present entry does not invalidate anything -- and a
// KTEST written for it passed with the call commented out. It was
// deleted rather than committed looking green; the reasoning lives in
// comp_map()'s comment instead, which is where an edit would meet it.
//
// The reads and writes below go through vmm_copy_to_user() /
// vmm_copy_from_user() against a specific address space. That is not
// just the sanctioned way for kernel code to touch user memory (vmm.h)
// -- it is also the strongest available form of the assertion, because
// those helpers WALK THE PAGE TABLES to reach the frame. A successful
// copy proves the mapping resolves in that address space; a failed one
// proves it does not. Nothing here dereferences a user pointer, so
// nothing here depends on which CR3 is loaded.
#include "ktest.h"
#include "win_server.h"
#include "vmm.h"
#include <stddef.h>

// Two synthetic pids, both inside WIN_SERVER_MAX_PIDS. Deliberately not
// 1: pid 1 is what a real spawned client tends to get, and these tests
// run in the live kernel where a desktop may be up.
#define CLIENT_PID 3
#define COMP_PID   4

#define WIN_W 64
#define WIN_H 32

// A fixture: two address spaces, one window. Every test needs the same
// three lines and the same teardown, and a leaked address space here is
// a leak in the live kernel the suite is running inside.
struct fixture {
    uint64_t client_as;
    uint64_t comp_as;
    uint32_t id;
};

static int fixture_up(struct fixture *f) {
    f->client_as = vmm_create_address_space();
    f->comp_as = vmm_create_address_space();
    if (!f->client_as || !f->comp_as) return 0;
    if (!win_server_create_raw(CLIENT_PID, f->client_as, WIN_W, WIN_H, &f->id)) return 0;
    if (!win_server_set_compositor(COMP_PID, f->comp_as)) return 0;
    return 1;
}

static void fixture_down(struct fixture *f) {
    win_server_destroy_raw(CLIENT_PID, f->id);
    // Clear the registration before the address space goes away, so no
    // stale pml4 is left registered for the next test (or for the live
    // desktop, which shares this kernel).
    win_server_set_compositor(0, 0);
    if (f->client_as) vmm_destroy_address_space(f->client_as);
    if (f->comp_as) vmm_destroy_address_space(f->comp_as);
}

KTEST("winshare", "a window maps into the compositor at its derived address") {
    struct fixture f = {0};
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t vaddr = 0;
    KTEST_ASSERT(win_server_map_to_compositor(COMP_PID, CLIENT_PID, f.id, &vaddr));
    // The address is DERIVED, so the server and the compositor cannot
    // disagree about it -- this asserts the returned value is the one
    // the formula gives rather than something the server chose.
    KTEST_ASSERT_EQ(vaddr, win_compositor_vaddr(CLIENT_PID, f.id));
    KTEST_ASSERT(win_server_is_mapped_to_compositor(CLIENT_PID, f.id));
    KTEST_ASSERT(vmm_validate_user_range(f.comp_as, vaddr, 4096));

    fixture_down(&f);
}

KTEST("winshare", "both address spaces see the SAME pixels, not a copy") {
    struct fixture f = {0};
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t cvaddr = 0;
    KTEST_ASSERT(win_server_map_to_compositor(COMP_PID, CLIENT_PID, f.id, &cvaddr));

    // Write as the CLIENT would (into its own mapping), read as the
    // compositor. A copy would pass a same-value check made any other
    // way; going through two different address spaces is what makes
    // this about sharing rather than about memory working.
    uint32_t pixel = 0xC0FFEE01;
    KTEST_ASSERT(vmm_copy_to_user(f.client_as, win_buffer_vaddr(f.id),
                                   &pixel, sizeof pixel));
    uint32_t seen = 0;
    KTEST_ASSERT(vmm_copy_from_user(f.comp_as, &seen, cvaddr, sizeof seen));
    KTEST_ASSERT_EQ(seen, 0xC0FFEE01);

    // And the other direction: a compositor that only ever reads is the
    // normal case, but the mapping is writable and a one-way test would
    // not notice if it silently were not.
    pixel = 0x0BADF00D;
    KTEST_ASSERT(vmm_copy_to_user(f.comp_as, cvaddr, &pixel, sizeof pixel));
    seen = 0;
    KTEST_ASSERT(vmm_copy_from_user(f.client_as, &seen,
                                     win_buffer_vaddr(f.id), sizeof seen));
    KTEST_ASSERT_EQ(seen, 0x0BADF00D);

    fixture_down(&f);
}

// THE ONE THAT MATTERS. A revocation bug leaves the compositor reading
// frames the allocator has already handed to something else.
//
// What "revoked" means changed with comp_poison() (win_server.c): the
// slot stays MAPPED, at the shared zero page, because a compositor is a
// process that learns of the death from a queued event and may blit the
// slot once more before it does -- and a hole there is a page fault,
// i.e. the desktop dying. So the assertion is not "nothing is mapped"
// but "the client's pixels are gone", which is the property that was
// ever worth having.
KTEST("winshare", "destroying a window poisons the compositor's mapping") {
    struct fixture f = {0};
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t cvaddr = 0;
    KTEST_ASSERT(win_server_map_to_compositor(COMP_PID, CLIENT_PID, f.id, &cvaddr));
    KTEST_ASSERT(vmm_validate_user_range(f.comp_as, cvaddr, 4096));

    // A marker the compositor can only still see through the ORIGINAL
    // frames, so "reads zero" below distinguishes a poisoned slot from
    // one left pointing at freed memory. Without it the check passes
    // against the use-after-free it exists to rule out.
    uint32_t marker = 0xDEADBEEF;
    KTEST_ASSERT(vmm_copy_to_user(f.client_as, win_buffer_vaddr(f.id),
                                   &marker, sizeof marker));

    KTEST_ASSERT(win_server_destroy_raw(CLIENT_PID, f.id));

    // Ask the PAGE TABLES, not the flag -- the flag is the thing that
    // would be wrong if this were broken. Still mapped (no fault for a
    // compositor mid-frame), and no longer the client's pixels.
    KTEST_ASSERT(vmm_validate_user_range(f.comp_as, cvaddr, 4096));
    uint32_t seen = 0xFFFFFFFF;
    KTEST_ASSERT(vmm_copy_from_user(f.comp_as, &seen, cvaddr, sizeof seen));
    KTEST_ASSERT_EQ(seen, 0);
    KTEST_ASSERT(!win_server_is_mapped_to_compositor(CLIENT_PID, f.id));

    f.id = 0; // already destroyed; don't destroy a live window's slot
    win_server_set_compositor(0, 0);
    if (f.client_as) vmm_destroy_address_space(f.client_as);
    if (f.comp_as) vmm_destroy_address_space(f.comp_as);
}

KTEST("winshare", "a client dying revokes it too") {
    struct fixture f = {0};
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t cvaddr = 0;
    KTEST_ASSERT(win_server_map_to_compositor(COMP_PID, CLIENT_PID, f.id, &cvaddr));

    // The path a real crash takes, which is NOT the same code as an
    // orderly WIN_REQ_DESTROY -- process teardown calls this directly.
    uint32_t marker = 0xDEADBEEF;
    KTEST_ASSERT(vmm_copy_to_user(f.client_as, win_buffer_vaddr(f.id),
                                   &marker, sizeof marker));

    win_server_client_gone(CLIENT_PID);

    // Poisoned, not unmapped -- see the previous test's comment. This is
    // the path a force quit takes, where the compositor is the process
    // that ASKED for the kill and returns from the syscall still holding
    // the dead window in its list.
    KTEST_ASSERT(vmm_validate_user_range(f.comp_as, cvaddr, 4096));
    uint32_t seen = 0xFFFFFFFF;
    KTEST_ASSERT(vmm_copy_from_user(f.comp_as, &seen, cvaddr, sizeof seen));
    KTEST_ASSERT_EQ(seen, 0);
    KTEST_ASSERT_EQ(win_server_window_count(CLIENT_PID), 0);

    f.id = 0;
    win_server_set_compositor(0, 0);
    if (f.client_as) vmm_destroy_address_space(f.client_as);
    if (f.comp_as) vmm_destroy_address_space(f.comp_as);
}

KTEST("winshare", "a resize re-points the mapping at the NEW frames") {
    struct fixture f = {0};
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t cvaddr = 0;
    KTEST_ASSERT(win_server_map_to_compositor(COMP_PID, CLIENT_PID, f.id, &cvaddr));

    // Mark the OLD buffer. After the resize this value must be gone --
    // if it is still readable through the compositor's mapping, the
    // mapping is still pointing at frames the resize freed, which is
    // precisely the use-after-free this test exists for.
    uint32_t marker = 0xDEADBEEF;
    KTEST_ASSERT(vmm_copy_to_user(f.comp_as, cvaddr, &marker, sizeof marker));

    // Not through win_server_request(): that refuses everything when no
    // presentation layer is registered, and a `ktest` run has no
    // desktop. This is the same resize_window() the protocol calls.
    KTEST_ASSERT(win_server_resize_raw(CLIENT_PID, f.id, WIN_W * 3, WIN_H * 3));

    // Still mapped, still at the same address -- that is the whole
    // point of deriving it (a compositor is never told pixels moved).
    KTEST_ASSERT(win_server_is_mapped_to_compositor(CLIENT_PID, f.id));
    KTEST_ASSERT(vmm_validate_user_range(f.comp_as, cvaddr, 4096));

    // A fresh buffer is zeroed, so the marker cannot survive unless the
    // mapping never moved.
    uint32_t seen = 0xFFFFFFFF;
    KTEST_ASSERT(vmm_copy_from_user(f.comp_as, &seen, cvaddr, sizeof seen));
    KTEST_ASSERT_EQ(seen, 0);

    // And it is genuinely the new buffer: write through the client's
    // view and see it through the compositor's.
    uint32_t pixel = 0x11223344;
    KTEST_ASSERT(vmm_copy_to_user(f.client_as, win_buffer_vaddr(f.id),
                                   &pixel, sizeof pixel));
    KTEST_ASSERT(vmm_copy_from_user(f.comp_as, &seen, cvaddr, sizeof seen));
    KTEST_ASSERT_EQ(seen, 0x11223344);

    fixture_down(&f);
}

KTEST("winshare", "only the registered compositor may map a window") {
    struct fixture f = {0};
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t vaddr = 0;
    // A process that is not the compositor, asking for someone else's
    // pixels. This is the request that must never succeed -- a window
    // buffer is private memory, and mapping it into an arbitrary
    // process is a hole rather than a feature.
    KTEST_ASSERT(!win_server_map_to_compositor(CLIENT_PID, CLIENT_PID, f.id, &vaddr));
    KTEST_ASSERT(!win_server_is_mapped_to_compositor(CLIENT_PID, f.id));

    // Nor may the compositor map a window that does not exist.
    KTEST_ASSERT(!win_server_map_to_compositor(COMP_PID, CLIENT_PID, WIN_CLIENT_MAX, &vaddr));
    // Nor one belonging to a pid outside the table.
    KTEST_ASSERT(!win_server_map_to_compositor(COMP_PID, 0, 0, &vaddr));

    fixture_down(&f);
}

KTEST("winshare", "clearing the compositor drops its mappings") {
    struct fixture f = {0};
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t cvaddr = 0;
    KTEST_ASSERT(win_server_map_to_compositor(COMP_PID, CLIENT_PID, f.id, &cvaddr));

    // A compositor exiting is the case this protects: its address space
    // is about to be destroyed, and a mapping flag left set would make
    // the next unmap walk a pml4 that no longer exists.
    KTEST_ASSERT(win_server_set_compositor(0, 0));
    KTEST_ASSERT(!win_server_is_mapped_to_compositor(CLIENT_PID, f.id));
    KTEST_ASSERT(!vmm_validate_user_range(f.comp_as, cvaddr, 4096));

    // And a map request with nobody registered is refused rather than
    // mapping into whatever pml4 was there last.
    KTEST_ASSERT(!win_server_map_to_compositor(COMP_PID, CLIENT_PID, f.id, &cvaddr));

    fixture_down(&f);
}

// --- the way IN to all of the above (M41 stage 2) ---------------------
//
// Everything above calls win_server_set_compositor() directly, which
// nothing outside this file could do: the registration existed with no
// syscall and no message reaching it. These cover the message that fills
// that gap, and the two rules around it.
//
// They deliberately do NOT go through the syscall: what is interesting
// here is win_server_request()'s own handling, and driving it directly
// keeps the test free of a user address space it does not need.

KTEST("winshare", "WIN_REQ_SET_COMPOSITOR claims and releases the role") {
    struct win_request_msg req = {0};

    req.type = WIN_REQ_SET_COMPOSITOR;
    req.a = 1;
    KTEST_ASSERT_EQ(win_server_request(COMP_PID, &req), 1);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), COMP_PID);

    req.a = 0;
    KTEST_ASSERT_EQ(win_server_request(COMP_PID, &req), 1);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), 0);
}

KTEST("winshare", "only the holder may release the compositor role") {
    struct win_request_msg req = {0};
    req.type = WIN_REQ_SET_COMPOSITOR;

    req.a = 1;
    KTEST_ASSERT_EQ(win_server_request(COMP_PID, &req), 1);

    // Without this check any process could evict the compositor, taking
    // the raw input stream and every buffer mapping down with it -- a
    // denial of service that needs no privilege at all.
    req.a = 0;
    KTEST_ASSERT_EQ(win_server_request(COMP_PID + 1, &req), 0);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), COMP_PID);

    KTEST_ASSERT_EQ(win_server_request(COMP_PID, &req), 1);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), 0);
}

KTEST("winshare", "claiming needs no registered presentation layer") {
    // THE stage-4 property: the ring-3 WM is itself the compositor, so
    // there is no kernel-side presentation layer for it to wait on. Every
    // other request is refused outright when none is registered; this one
    // must not be. Skipped when the desktop is up, since the condition
    // under test is then not present -- `make test` runs before GUI mode,
    // which is when this actually means something.
    // EITHER KIND OF SERVER counts. This guard read `win_server_active()`
    // alone, which answers "is a RING-0 presentation layer registered" --
    // and the desktop stopped being one when it became a ring-3
    // compositor, so the skip silently stopped firing while its comment
    // went on claiming it did. Nothing noticed until init started the
    // desktop at boot (docs/init-design.md stage 2) and `make test`
    // finally ran with one up.
    if (win_server_any()) KTEST_SKIP("desktop is up -- no !g_ops regime to test");

    struct win_request_msg req = {0};
    req.type = WIN_REQ_PRESENT;
    KTEST_ASSERT_EQ(win_server_request(COMP_PID, &req), -1); // the control

    req.type = WIN_REQ_SET_COMPOSITOR;
    req.a = 1;
    KTEST_ASSERT_EQ(win_server_request(COMP_PID, &req), 1);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), COMP_PID);

    req.a = 0;
    KTEST_ASSERT_EQ(win_server_request(COMP_PID, &req), 1);
}

