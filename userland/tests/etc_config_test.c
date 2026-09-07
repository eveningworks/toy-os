// The /etc config parser, in RING 3.
//
// WHY THIS EXISTS RATHER THAN ONLY A KTEST -- the same reason
// klineedit_test.c does. kernel/lib/etc_config.c has KTESTs, and they
// would go on passing whether or not a single line of it were reachable
// from a ring-3 program. What is new is the LINK: the parser is
// compiled a second time with USERLAND_CFLAGS into libuapp.a, which is
// what /bin/netd, the File Manager and the window manager read /etc
// through, and a KTEST runs inside the kernel and cannot see that build.
//
// So the assertion is not "the parser works". It is "the ring-3 build
// of it produces byte-for-byte what the kernel's build produces", over
// the table both rings read (kernel/include/api/etc_config_cases.h).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "etc_config.h"
#include "etc_config_cases.h"
#include "lib/utest.h"

// STATIC: a struct etc_config_buf is 4 KiB and the rewrite buffer
// another 4, against USERLAND_CFLAGS' 2 KiB frame budget -- a local
// that size does not merely overflow, it steps over the single guard
// page below the stack.
static struct etc_config_buf g_buf;
static char g_out[ETC_CONFIG_MAX];
static char g_got[256];

int main(void) {
    utest_begin("etc_config_test", "the shared /etc parser, built for ring 3", 0);
    char detail[320];

    for (int i = 0; i < etc_get_case_count; i++) {
        int ok = etc_get_case_run(&etc_get_cases[i], &g_buf, g_got, sizeof g_got);
        snprintf(detail, sizeof detail, "got \"%s\" want \"%s\"",
                 g_got, etc_get_cases[i].want ? etc_get_cases[i].want : "(a miss)");
        utest_check_detail(ok, etc_get_cases[i].name, detail);
    }

    for (int i = 0; i < etc_set_case_count; i++) {
        int ok = etc_set_case_run(&etc_set_cases[i], g_out, sizeof g_out,
                                  g_got, sizeof g_got);
        snprintf(detail, sizeof detail, "got \"%s\" want \"%s\"",
                 g_got, etc_set_cases[i].want ? etc_set_cases[i].want : "(a refusal)");
        utest_check_detail(ok, etc_set_cases[i].name, detail);
    }

    for (int i = 0; i < etc_sections_case_count; i++) {
        int ok = etc_sections_case_run(&etc_sections_cases[i], &g_buf,
                                       g_got, sizeof g_got);
        snprintf(detail, sizeof detail, "got \"%s\" want \"%s\"",
                 g_got, etc_sections_cases[i].want);
        utest_check_detail(ok, etc_sections_cases[i].name, detail);
    }

    // An EMPTY table would print nothing and exit 0, which reads exactly
    // like every case passing.
    utest_checkf(etc_get_case_count >= 10 && etc_set_case_count >= 10
                 && etc_sections_case_count >= 4,
                 "%d/%d/%d cases -- the shared table is not linked",
                 etc_get_case_count, etc_set_case_count, etc_sections_case_count);

    return utest_end();
}
