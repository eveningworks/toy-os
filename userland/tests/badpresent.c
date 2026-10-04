// A raw-protocol client that re-presents its ON-SCREEN buffer at a new
// generation and a size its object cannot hold. The compositor must keep
// the last good frame: it used to drop the old mapping before mapping the
// replacement, so the refused replacement left it blitting from a hole
// and it page-faulted. No uapp -- uapp never re-presents its front buffer,
// which is exactly why this path was never reached.
//
// Driven by tools/bad_present_test.py; it logs each step and then sits,
// draining nothing, until it is killed.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "win_proto.h"
#include "lib/uchan.h"
#include "lib/uwmchan.h"

#define W 200
#define H 120

static int send(struct uchan_client *c, uint32_t type, uint32_t win, int a, int b) {
    struct wmchan_msg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.window = win;
    m.a = a;
    m.b = b;
    return uchan_send(c, &m, sizeof m);
}

int main(void) {
    struct uchan_client c;
    int i = 0;
    while (uchan_client_open(&c, WMCHAN_SERVICE) != 0) {
        if (++i > 200) { sys_eprint("badpresent: no compositor channel\n"); return 1; }
        sys_sleep_ms(10);
    }

    uint64_t bytes = ((uint64_t)W * H * 4 + 4095) & ~4095ULL;
    char nm[WIN_BUF_NAME_MAX];
    snprintf(nm, sizeof nm, WIN_BUF_NAME_FMT, sys_getpid(), 0, 0);
    int fd = sys_shm_open(nm, bytes, SHM_CREATE | SHM_EXCL);
    if (fd < 0) { sys_eprint("badpresent: no buffer\n"); return 1; }
    sys_shm_grant(nm, c.beacon->server_pid);
    uint32_t *px = sys_mmap(0, bytes, SYS_PROT_READ | SYS_PROT_WRITE, SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    if (px == (void *)-1) { sys_eprint("badpresent: no mapping\n"); return 1; }
    for (int k = 0; k < W * H; k++) px[k] = 0x00C04030;

    struct wmchan_msg m, r;
    memset(&m, 0, sizeof m);
    m.type = WIN_REQ_CREATE;
    m.a = W;
    m.b = H;
    snprintf(m.text, sizeof m.text, "badpresent");
    if (uchan_call(&c, &m, sizeof m, &r, sizeof r, 3000) < 0 || r.a < 0) {
        sys_eprint("badpresent: create refused\n");
        return 1;
    }
    uint32_t win = (uint32_t)r.a;
    memset(&m, 0, sizeof m);
    m.type = WIN_REQ_TITLE;
    m.window = win;
    snprintf(m.text, sizeof m.text, "Bad Present");
    uchan_send(&c, &m, sizeof m);

    send(&c, WIN_REQ_PRESENT, win, WIN_PRESENT_B(0, 1), (int)WIN_PRESENT_SIZE(W, H));
    sys_eprint("badpresent: presented\n");
    sys_sleep_ms(1500);

    // The SAME buffer, now on screen, claimed at a new generation and a
    // size its object cannot hold: the compositor's re-open maps nothing.
    send(&c, WIN_REQ_PRESENT, win, WIN_PRESENT_B(0, 2), (int)WIN_PRESENT_SIZE(4000, 4000));
    sys_eprint("badpresent: bad present sent\n");

    for (;;) sys_sleep_ms(1000);
}
