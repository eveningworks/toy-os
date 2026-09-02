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
#include "pmm.h"
#include "scheduler.h"
#include <stddef.h>

// The pids are CHOSEN AT RUN TIME, not named. Every window-server pid
// is one a real process can hold -- WIN_SERVER_MAX_PIDS is
// SCHED_MAX_PROCS -- so there is no synthetic number that is safe, and
// pid 3 (what this file used to hardcode) is what toywm gets on an
// ordinary boot: the fixture was creating and destroying windows on the
// live desktop's own list.
static int spare_pids(int *client, int *comp) {
    *client = *comp = 0;
    for (int p = SCHED_MAX_PROCS - 1; p > 0; p--) {
        if (scheduler_pid_valid(p)) continue; // valid counts a zombie, which is what we want
        if (!*client) { *client = p; continue; }
        *comp = p;
        return 1;
    }
    return 0;
}

// THE COMPOSITOR ROLE IS ONE GLOBAL, SHARED WITH THE LIVE DESKTOP.
// Taking it revokes that desktop's framebuffer grant and drops every
// mapping it holds (win_server_set_compositor()); giving it up asks its
// clients to close and toywm exits. Neither is undoable by restoring
// the role afterwards, because the damage is done on the way IN. So
// these refuse to run while anyone holds it, and tools/ktest_run.py
// frees the role before the suite -- which is how the gate still
// exercises them without the default graphical boot destroying itself.
#define SKIP_IF_ROLE_HELD                                                     \
    do {                                                                      \
        if (win_server_compositor_pid())                                      \
            KTEST_SKIP("a compositor holds the role");                        \
    } while (0)

#define WIN_W 64
#define WIN_H 32

// A fixture: two address spaces, one window. Every test needs the same
// three lines and the same teardown, and a leaked address space here is
// a leak in the live kernel the suite is running inside.
struct fixture {
    uint64_t client_as;
    uint64_t comp_as;
    uint32_t id;
    int client_pid;
    int comp_pid;
};

static int fixture_up(struct fixture *f) {
    if (!spare_pids(&f->client_pid, &f->comp_pid)) return 0;
    f->client_as = vmm_create_address_space();
    f->comp_as = vmm_create_address_space();
    if (!f->client_as || !f->comp_as) return 0;
    if (!win_server_create_raw(f->client_pid, f->client_as, WIN_W, WIN_H, &f->id)) return 0;
    if (!win_server_set_compositor(f->comp_pid, f->comp_as)) return 0;
    return 1;
}

static void fixture_down(struct fixture *f) {
    if (f->client_pid) win_server_destroy_raw(f->client_pid, f->id);
    // Clear the registration before the address space goes away, so no
    // stale pml4 is left registered for the next test. Safe to clear
    // unconditionally only because SKIP_IF_ROLE_HELD proved the role was
    // free before the fixture took it.
    win_server_set_compositor(0, 0);
    if (f->client_as) vmm_destroy_address_space(f->client_as);
    if (f->comp_as) vmm_destroy_address_space(f->comp_as);
}

// A window's pixels are CPU-only -- the compositor reads them, the
// client writes them, and nothing DMAs from them -- so they take frames
// from PMM_ZONE_ANY ("More than 4 GiB of RAM", stage 3). Asked of the
// page table rather than of pmm, so it fails if create_window() were
// changed back.
KTEST("winshare", "a window's buffer comes from the high zone") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (pmm_zone_free_frames(PMM_ZONE_ANY) == 0)
        KTEST_SKIP("guest has no memory above 4 GiB");
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t four_gib = (uint64_t)4 * 1024 * 1024 * 1024;
    uint64_t phys = vmm_user_phys(f.client_as, win_buffer_vaddr(f.id));
    fixture_down(&f);

    KTEST_ASSERT(phys != 0);
    KTEST_ASSERT(phys >= four_gib);
}

KTEST("winshare", "a window maps into the compositor at its derived address") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t vaddr = 0;
    KTEST_ASSERT(win_server_map_to_compositor(f.comp_pid, f.client_pid, f.id, &vaddr));
    // The address is DERIVED, so the server and the compositor cannot
    // disagree about it -- this asserts the returned value is the one
    // the formula gives rather than something the server chose.
    KTEST_ASSERT_EQ(vaddr, win_compositor_vaddr(f.client_pid, f.id));
    KTEST_ASSERT(win_server_is_mapped_to_compositor(f.client_pid, f.id));
    KTEST_ASSERT(vmm_validate_user_range(f.comp_as, vaddr, 4096));

    fixture_down(&f);
}

KTEST("winshare", "both address spaces see the SAME pixels, not a copy") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t cvaddr = 0;
    KTEST_ASSERT(win_server_map_to_compositor(f.comp_pid, f.client_pid, f.id, &cvaddr));

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

// THE TEARING INVARIANT: while a window has two buffers, the one the
// client draws into is never the one the compositor reads.
//
// Asserted as MEMORY rather than as a flicker, deliberately. Catching a
// torn frame means sampling the screen fast enough to land inside one
// client redraw, which is timing-dependent, flaky, and gets FASTER to
// miss as the machine gets quicker -- a check that passes more often
// the less it is true. What actually has to hold is that two pointers
// differ, and that is decidable.
KTEST("winshare", "a present flips the buffer, so the two never collide") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t cvaddr = 0;
    KTEST_ASSERT(win_server_map_to_compositor(f.comp_pid, f.client_pid, f.id, &cvaddr));

    // A window starts at front 0, so the client draws into buffer 1.
    // Both offsets come from the ABI's own helpers -- if a caller
    // computed them by hand the test would be checking its own
    // arithmetic rather than the contract.
    KTEST_ASSERT(win_buffer_front_offset(0) != win_buffer_back_offset(0));
    KTEST_ASSERT(win_buffer_front_offset(1) != win_buffer_back_offset(1));

    // Write a marker into the BACK buffer as the client, and confirm the
    // compositor's FRONT view does not see it. This is the whole
    // property: a half-finished frame is invisible until it is
    // presented.
    uint32_t drawing = 0xDEADBEEF;
    KTEST_ASSERT(vmm_copy_to_user(f.client_as,
                                   win_buffer_vaddr(f.id) + win_buffer_back_offset(0),
                                   &drawing, sizeof drawing));
    uint32_t seen = 0xFFFFFFFF;
    KTEST_ASSERT(vmm_copy_from_user(f.comp_as, &seen,
                                     cvaddr + win_buffer_front_offset(0),
                                     sizeof seen));
    KTEST_ASSERT(seen != 0xDEADBEEF);

    // Now present. The server flips, and the marker becomes visible
    // through the compositor's front view -- the same bytes, reached
    // from the other address space, which is what makes this about the
    // FLIP rather than about two unrelated pages.
    struct win_request_msg req;
    for (unsigned i = 0; i < sizeof req; i++) ((uint8_t *)&req)[i] = 0;
    req.type = WIN_REQ_PRESENT;
    req.window = f.id;
    int rc = win_server_request(f.client_pid, &req);
    // 1 or 2: the new front index, biased so 0 still means refused.
    KTEST_ASSERT(rc > 0);
    int front = rc - 1;

    KTEST_ASSERT(vmm_copy_from_user(f.comp_as, &seen,
                                     cvaddr + win_buffer_front_offset(front),
                                     sizeof seen));
    KTEST_ASSERT_EQ(seen, 0xDEADBEEF);

    // ...and the client is now pointed somewhere ELSE, which is the
    // half that stops the next frame landing on the one being read.
    KTEST_ASSERT(win_buffer_back_offset(front) != win_buffer_front_offset(front));

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
// THE CHECK THAT ASKS THE WHOLE SLOT, and the one whose absence let the
// second buffer go un-revoked for months. Every other check here names
// ONE address, and the fixture's windows are a single page, so a range
// nobody names is a range nobody tests -- while comp_map() has always
// mapped a second one at +WIN_BUFFER_HALF. vmm_audit_space() needs no
// address at all: it walks the compositor's whole address space and
// reports any mapping pointing at a frame the allocator has taken back.
//
// Positive control: drop the `for (int b ...)` loop in comp_clear() back
// to buffer 0 and this goes red with `dangling` equal to the window's
// page count, while every other check here stays green.
KTEST("winshare", "destroying a window leaves the compositor no mapping of a freed frame") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t cvaddr = 0;
    KTEST_ASSERT(win_server_map_to_compositor(f.comp_pid, f.client_pid, f.id, &cvaddr));
    // Both halves of the slot are mapped while the window lives -- which
    // is what makes the second one something that has to be revoked.
    KTEST_ASSERT(vmm_validate_user_range(f.comp_as, cvaddr, 4096));
    KTEST_ASSERT(vmm_validate_user_range(f.comp_as, cvaddr + WIN_BUFFER_HALF, 4096));

    KTEST_ASSERT(win_server_destroy_raw(f.client_pid, f.id));

    struct vmm_audit a;
    uint64_t dangling = vmm_audit_space(f.comp_as, &a);

    // And the second half reads as poison rather than as the dead
    // window's pixels, the same contract the first half has.
    uint32_t seen2 = 0xFFFFFFFF;
    int got2 = vmm_copy_from_user(f.comp_as, &seen2, cvaddr + WIN_BUFFER_HALF, sizeof seen2);

    f.id = 0;
    win_server_set_compositor(0, 0);
    if (f.client_as) vmm_destroy_address_space(f.client_as);
    if (f.comp_as) vmm_destroy_address_space(f.comp_as);

    KTEST_ASSERT_EQ((int)dangling, 0);
    KTEST_ASSERT(got2);
    KTEST_ASSERT_EQ(seen2, 0);
}

KTEST("winshare", "destroying a window poisons the compositor's mapping") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t cvaddr = 0;
    KTEST_ASSERT(win_server_map_to_compositor(f.comp_pid, f.client_pid, f.id, &cvaddr));
    KTEST_ASSERT(vmm_validate_user_range(f.comp_as, cvaddr, 4096));

    // A marker the compositor can only still see through the ORIGINAL
    // frames, so "reads zero" below distinguishes a poisoned slot from
    // one left pointing at freed memory. Without it the check passes
    // against the use-after-free it exists to rule out.
    uint32_t marker = 0xDEADBEEF;
    KTEST_ASSERT(vmm_copy_to_user(f.client_as, win_buffer_vaddr(f.id),
                                   &marker, sizeof marker));

    KTEST_ASSERT(win_server_destroy_raw(f.client_pid, f.id));

    // Ask the PAGE TABLES, not the flag -- the flag is the thing that
    // would be wrong if this were broken. Still mapped (no fault for a
    // compositor mid-frame), and no longer the client's pixels.
    KTEST_ASSERT(vmm_validate_user_range(f.comp_as, cvaddr, 4096));
    uint32_t seen = 0xFFFFFFFF;
    KTEST_ASSERT(vmm_copy_from_user(f.comp_as, &seen, cvaddr, sizeof seen));
    KTEST_ASSERT_EQ(seen, 0);
    KTEST_ASSERT(!win_server_is_mapped_to_compositor(f.client_pid, f.id));

    f.id = 0; // already destroyed; don't destroy a live window's slot
    win_server_set_compositor(0, 0);
    if (f.client_as) vmm_destroy_address_space(f.client_as);
    if (f.comp_as) vmm_destroy_address_space(f.comp_as);
}

KTEST("winshare", "a client dying revokes it too") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t cvaddr = 0;
    KTEST_ASSERT(win_server_map_to_compositor(f.comp_pid, f.client_pid, f.id, &cvaddr));

    // The path a real crash takes, which is NOT the same code as an
    // orderly WIN_REQ_DESTROY -- process teardown calls this directly.
    uint32_t marker = 0xDEADBEEF;
    KTEST_ASSERT(vmm_copy_to_user(f.client_as, win_buffer_vaddr(f.id),
                                   &marker, sizeof marker));

    win_server_client_gone(f.client_pid);

    // Poisoned, not unmapped -- see the previous test's comment. This is
    // the path a force quit takes, where the compositor is the process
    // that ASKED for the kill and returns from the syscall still holding
    // the dead window in its list.
    KTEST_ASSERT(vmm_validate_user_range(f.comp_as, cvaddr, 4096));
    uint32_t seen = 0xFFFFFFFF;
    KTEST_ASSERT(vmm_copy_from_user(f.comp_as, &seen, cvaddr, sizeof seen));
    KTEST_ASSERT_EQ(seen, 0);
    KTEST_ASSERT_EQ(win_server_window_count(f.client_pid), 0);

    f.id = 0;
    win_server_set_compositor(0, 0);
    if (f.client_as) vmm_destroy_address_space(f.client_as);
    if (f.comp_as) vmm_destroy_address_space(f.comp_as);
}

KTEST("winshare", "a resize re-points the mapping at the NEW frames") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t cvaddr = 0;
    KTEST_ASSERT(win_server_map_to_compositor(f.comp_pid, f.client_pid, f.id, &cvaddr));

    // Mark the OLD buffer. After the resize this value must be gone --
    // if it is still readable through the compositor's mapping, the
    // mapping is still pointing at frames the resize freed, which is
    // precisely the use-after-free this test exists for.
    uint32_t marker = 0xDEADBEEF;
    KTEST_ASSERT(vmm_copy_to_user(f.comp_as, cvaddr, &marker, sizeof marker));

    // Not through win_server_request(): that refuses everything when no
    // presentation layer is registered, and a `ktest` run has no
    // desktop. This is the same resize_window() the protocol calls.
    KTEST_ASSERT(win_server_resize_raw(f.client_pid, f.id, WIN_W * 3, WIN_H * 3));

    // Still mapped, still at the same address -- that is the whole
    // point of deriving it (a compositor is never told pixels moved).
    KTEST_ASSERT(win_server_is_mapped_to_compositor(f.client_pid, f.id));
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
    SKIP_IF_ROLE_HELD;
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t vaddr = 0;
    // A process that is not the compositor, asking for someone else's
    // pixels. This is the request that must never succeed -- a window
    // buffer is private memory, and mapping it into an arbitrary
    // process is a hole rather than a feature.
    KTEST_ASSERT(!win_server_map_to_compositor(f.client_pid, f.client_pid, f.id, &vaddr));
    KTEST_ASSERT(!win_server_is_mapped_to_compositor(f.client_pid, f.id));

    // Nor may the compositor map a window that does not exist.
    KTEST_ASSERT(!win_server_map_to_compositor(f.comp_pid, f.client_pid, WIN_CLIENT_MAX, &vaddr));
    // Nor one belonging to a pid outside the table.
    KTEST_ASSERT(!win_server_map_to_compositor(f.comp_pid, 0, 0, &vaddr));

    fixture_down(&f);
}

KTEST("winshare", "clearing the compositor drops its mappings") {
    struct fixture f = {0};
    SKIP_IF_ROLE_HELD;
    if (!fixture_up(&f)) { fixture_down(&f); KTEST_SKIP("out of memory"); }

    uint64_t cvaddr = 0;
    KTEST_ASSERT(win_server_map_to_compositor(f.comp_pid, f.client_pid, f.id, &cvaddr));

    // A compositor exiting is the case this protects: its address space
    // is about to be destroyed, and a mapping flag left set would make
    // the next unmap walk a pml4 that no longer exists.
    KTEST_ASSERT(win_server_set_compositor(0, 0));
    KTEST_ASSERT(!win_server_is_mapped_to_compositor(f.client_pid, f.id));
    KTEST_ASSERT(!vmm_validate_user_range(f.comp_as, cvaddr, 4096));

    // And a map request with nobody registered is refused rather than
    // mapping into whatever pml4 was there last.
    KTEST_ASSERT(!win_server_map_to_compositor(f.comp_pid, f.client_pid, f.id, &cvaddr));

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
    int cpid, other;
    SKIP_IF_ROLE_HELD;
    if (!spare_pids(&cpid, &other)) KTEST_SKIP("no unused pids");

    // Preemption off for the body -- see the sibling test below
    // ("only the holder may release the compositor role") for the full
    // reasoning. Short version: the compositor role is a single global,
    // so anything else claiming it mid-body makes "after releasing,
    // nobody holds it" read somebody else's pid rather than 0. The skip
    // above means this no longer EVICTS a live desktop to run; it still
    // has to keep the role to itself while it runs.
    scheduler_preempt_disable();

    req.type = WIN_REQ_SET_COMPOSITOR;
    req.a = 1;
    KTEST_ASSERT_EQ(win_server_request(cpid, &req), 1);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), cpid);

    req.a = 0;
    KTEST_ASSERT_EQ(win_server_request(cpid, &req), 1);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), 0);

    scheduler_preempt_enable();
}

KTEST("winshare", "only the holder may release the compositor role") {
    struct win_request_msg req = {0};
    int cpid, other;
    SKIP_IF_ROLE_HELD;
    if (!spare_pids(&cpid, &other)) KTEST_SKIP("no unused pids");
    req.type = WIN_REQ_SET_COMPOSITOR;

    // PREEMPTION OFF FOR THE WHOLE BODY, and this is a precondition
    // rather than tidiness. The compositor role is a single global, so
    // anything that claims it while this runs makes the closing
    // assertion -- "after the holder releases it, nobody holds it" --
    // read that claimant's pid instead of 0. It used to race the live
    // desktop, which this test EVICTED to run at all; the skip above
    // ended the eviction, and the race is still worth closing because
    // the role is still shared.
    //
    // It passed for a long time on timing luck alone. What exposed it
    // was an unrelated change adding PCI KTESTs, whose thousands of
    // extra config-space port reads shifted boot timing enough to lose
    // the race about half the time; HEAD measured 4 runs in 4 clean and
    // the same suite with those tests present failed ~4 in 7.
    //
    // Disabling preemption ESTABLISHES the precondition instead of
    // loosening the assertion, which keeps the property under test
    // exactly as strong as it was -- the same call, and the same
    // reasoning, as the heap KTESTs that compare against a snapshot.
    scheduler_preempt_disable();

    req.a = 1;
    KTEST_ASSERT_EQ(win_server_request(cpid, &req), 1);

    // Without this check any process could evict the compositor, taking
    // the raw input stream and every buffer mapping down with it -- a
    // denial of service that needs no privilege at all.
    req.a = 0;
    KTEST_ASSERT_EQ(win_server_request(other, &req), 0);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), cpid);

    KTEST_ASSERT_EQ(win_server_request(cpid, &req), 1);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), 0);

    scheduler_preempt_enable();
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

    int cpid, other;
    if (!spare_pids(&cpid, &other)) KTEST_SKIP("no unused pids");
    (void)other;
    struct win_request_msg req = {0};
    req.type = WIN_REQ_PRESENT;
    KTEST_ASSERT_EQ(win_server_request(cpid, &req), -1); // the control

    req.type = WIN_REQ_SET_COMPOSITOR;
    req.a = 1;
    KTEST_ASSERT_EQ(win_server_request(cpid, &req), 1);
    KTEST_ASSERT_EQ(win_server_compositor_pid(), cpid);

    req.a = 0;
    KTEST_ASSERT_EQ(win_server_request(cpid, &req), 1);
}

