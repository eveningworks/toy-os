// The framebuffer LEASE (docs/scanout-design.md): the grant's scanouts
// mapped into a second process, which then presents instead of the
// holder. On a fake holder and lessee, two address spaces nothing runs
// in, so the real compositor -- if one is up -- is not disturbed; the
// test SKIPS while somebody holds the grant.
#include "ktest.h"
#include "win_surface.h"
#include "vmm.h"
#include "gfx.h"
#include "scheduler.h"
#include "win_proto.h"


KTEST("winshare", "the framebuffer grant can be leased to one other process") {
    if (win_surface_holder()) KTEST_SKIP("a compositor holds the grant");
    if (!gfx_framebuffer_phys()) KTEST_SKIP("no framebuffer");
    int pids[2];
    if (!ktest_spare_pids(pids, 2)) KTEST_SKIP("no spare pids");
    int holder = pids[0], lessee = pids[1];
    uint64_t as1 = vmm_create_address_space(), as2 = vmm_create_address_space();
    KTEST_ASSERT(as1 && as2);

    uint32_t w, h, pitch, bpp; int count, back;
    KTEST_ASSERT(win_surface_grant(holder, as1, &w, &h, &pitch, &bpp, &count, &back));
    // Not to the holder itself.
    KTEST_ASSERT(!win_surface_lease(holder, as1, &w, &h, &pitch, &bpp, &count, &back));
    KTEST_ASSERT(win_surface_lease(lessee, as2, &w, &h, &pitch, &bpp, &count, &back));
    KTEST_ASSERT_EQ(win_surface_lessee(), lessee);
    KTEST_ASSERT_EQ(pitch, gfx_framebuffer_pitch());
    // The lessee sees the scanouts where the holder does.
    KTEST_ASSERT(vmm_user_phys(as2, WIN_FB_VADDR) == vmm_user_phys(as1, WIN_FB_VADDR));
    // One lease at a time.
    KTEST_ASSERT(!win_surface_lease(lessee, as2, &w, &h, &pitch, &bpp, &count, &back));
    // While it stands the LESSEE presents and the holder is refused.
    KTEST_ASSERT(!win_surface_present(holder, 0, 0, 8, 8, &back));
    KTEST_ASSERT(win_surface_present(lessee, 0, 0, 8, 8, &back));

    win_surface_lease_end(&back);
    KTEST_ASSERT_EQ(win_surface_lessee(), 0);
    // TWO PHASES: the display is the holder's again at once, but the
    // ex-lessee's pages stay until it has presented from its own buffer
    // -- it may be mid-frame into these (the DOOM crash).
    KTEST_ASSERT(vmm_user_phys(as2, WIN_FB_VADDR) != 0);
    win_surface_lease_unmap(holder);   // the wrong pid: a no-op
    KTEST_ASSERT(vmm_user_phys(as2, WIN_FB_VADDR) != 0);
    win_surface_lease_unmap(lessee);
    KTEST_ASSERT(vmm_user_phys(as2, WIN_FB_VADDR) == 0);
    KTEST_ASSERT(win_surface_present(holder, 0, 0, 8, 8, &back));
    KTEST_ASSERT(!win_surface_present(lessee, 0, 0, 8, 8, &back));

    // A lessee that dies ends its lease without its address space
    // being touched -- the mapping stays until the space is destroyed;
    // and a teardown reported by ADDRESS SPACE forgets an ex-lessee
    // too, so a deferred unmap can never write a dying space's tables.
    KTEST_ASSERT(win_surface_lease(lessee, as2, &w, &h, &pitch, &bpp, &count, &back));
    win_surface_client_gone(lessee);
    KTEST_ASSERT_EQ(win_surface_lessee(), 0);
    KTEST_ASSERT(vmm_user_phys(as2, WIN_FB_VADDR) != 0);
    KTEST_ASSERT(win_surface_lease(lessee, as2, &w, &h, &pitch, &bpp, &count, &back));
    win_surface_lease_end(&back);
    win_surface_space_gone(as2);
    win_surface_lease_unmap(lessee);   // forgotten: must NOT unmap
    KTEST_ASSERT(vmm_user_phys(as2, WIN_FB_VADDR) != 0);
    // Revoking the holder ends a lease too.
    KTEST_ASSERT(win_surface_lease(lessee, as2, &w, &h, &pitch, &bpp, &count, &back));
    win_surface_revoke(holder);
    KTEST_ASSERT_EQ(win_surface_lessee(), 0);
    KTEST_ASSERT_EQ(win_surface_holder(), 0);

    vmm_destroy_address_space(as1);
    vmm_destroy_address_space(as2);
}
