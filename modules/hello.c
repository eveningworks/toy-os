// hello -- the smallest loadable module: says so on load and unload,
// registers nothing. Proves the loader end to end (relocation against
// the export table, an initcall run at load, MODULE_EXIT at unload) and
// is what tools/module_test.py and the KTESTs load.
// driver-none: a test module, not a driver
#include "module.h"
#include "klog.h"
#include "kfmt.h"

static int g_loads;   // a data reference through movabs, relocated R_X86_64_64

static void hello_init(void) {
    g_loads++;
    klog_printf("hello: loaded (%d)\n", g_loads);
}
INITCALL(hello_init, INIT_CORE);

static void hello_exit(void) {
    klog_write("hello: unloaded\n");
}
MODULE_EXIT(hello_exit);
