// JSON library test -- wraps the pre-existing json_selftest() (parse,
// accessors, serialize, round-trip), which used to run on every boot.
#include "ktest.h"
#include "kapi.h"

KTEST("lib", "json parse/serialize round-trip (legacy selftest)") {
    KTEST_ASSERT(json_selftest() == 1);
}
