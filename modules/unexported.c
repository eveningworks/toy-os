// unexported -- a module that reaches for a kernel function the export
// table does not carry. It must be REFUSED at load with the symbol
// named; that refusal is what this file exists to test, so it never
// runs. tools/gen_modalias.py knows it by name and does not fail the
// build on its import.
// driver-none: a negative test module, not a driver
#include "module.h"
#include "klog.h"

int scheduler_kill(int pid, int exit_code);   // real, and deliberately not exported

static void unexported_init(void) {
    scheduler_kill(0, 0);
    klog_write("unexported: this line must never print\n");
}
INITCALL(unexported_init, INIT_CORE);
