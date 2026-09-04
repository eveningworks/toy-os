// SYS_QUERY, from ring 3.
//
// WHY THIS EXISTS RATHER THAN ONLY A KTEST. kernel/lib/query_test.c
// covers the registry -- registration, the two class shapes, field
// offsets -- and none of that needs a process. What is claimed HERE is
// that the answer survives the trip out: the message copied both ways,
// `len` genuinely truncating rather than the kernel writing past a short
// buffer, an errno arriving as an errno, and the discovery path working
// for a program that was told nothing but the number 0.
//
// THE LOAD-BEARING CHECK IS THE SHORT-BUFFER ONE. Version tolerance is
// the only reason `len` exists, and it is the kind of promise that is
// never exercised until the struct grows -- by which time the binary
// that needed it is already broken. So it is asked now, with a buffer
// deliberately smaller than the record, and the check is two-sided: the
// prefix must be RIGHT and the byte past `returned` must be UNTOUCHED.
//
// Prints one line per check and exits with the number of FAILURES.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>

#include "lib/utest.h"

static void check_errno(int got, int want, const char *what) {
    utest_checkf(got == want, "%s -- got %s%s%s", what, sys_strerror(got),
                 got != want ? ", wanted " : "", got != want ? sys_strerror(want) : "");
}

int main(void) {
    utest_begin("query_test", "facts, read from ring 3", 0);

    // --- the scalar class ---------------------------------------------
    struct query_meminfo m;
    int n = sys_query_record(QUERY_MEMINFO, 0, &m, sizeof m);
    utest_check(n == (int)sizeof m, "read the memory record whole");
    utest_check(m.frame_bytes >= 4096, "frame size is reported, not left 0");
    utest_check(m.frame_total > 0 && m.frame_free <= m.frame_total,
          "free frames never exceed the total");

    // --- version tolerance: a SHORT buffer truncates, it does not fail -
    // The byte after the buffer is a sentinel. A kernel that ignored
    // `len` and wrote the whole record would overwrite it, and the
    // prefix check alone would not notice.
    // THE BUFFER ITSELF IS SHORT, not merely the length argument. The
    // first version of this check handed the kernel a full-size struct
    // and a short `len`, so a kernel that ignored `len` wrote the whole
    // record and still fit -- the sentinel could not fire, and the
    // positive control proved it by reddening only the OTHER assertion.
    enum { SHORT_LEN = 24 }; // < sizeof(struct query_meminfo)
    struct {
        unsigned char partial[SHORT_LEN];
        uint64_t sentinel;
    } probe;
    memset(&probe, 0, sizeof probe);
    probe.sentinel = 0xA5A5A5A5A5A5A5A5ull;
    n = sys_query_record(QUERY_MEMINFO, 0, probe.partial, SHORT_LEN);
    utest_check(n == SHORT_LEN, "a short buffer reports how much it got");
    // The prefix must be RIGHT, not merely present: frame_total is the
    // record's first field, so a kernel writing the wrong offset would
    // still fill these bytes.
    uint64_t prefix = 0;
    memcpy(&prefix, probe.partial, sizeof prefix);
    utest_check(prefix == m.frame_total, "the prefix it did write is correct");
    utest_check(probe.sentinel == 0xA5A5A5A5A5A5A5A5ull,
          "and it wrote NOTHING past the length it was given");

    // --- the list class, and discovery from the number 0 --------------
    struct query_provider_info p;
    int providers = 0, saw_mem = 0, saw_self = 0;
    for (unsigned i = 0; ; i++) {
        if (sys_query_record(QUERY_PROVIDERS, i, &p, sizeof p) <= 0) break;
        providers++;
        if (strcmp(p.name, "mem") == 0) {
            saw_mem = 1;
            utest_check(!(p.flags & QUERY_F_LIST), "mem announces itself as a scalar");
            utest_check(p.record_size == sizeof(struct query_meminfo),
                  "mem's record size matches this build's struct");
        }
        if (p.cls == QUERY_PROVIDERS) saw_self = 1;
    }
    utest_check(providers >= 2, "walking class 0 found the registered providers");
    utest_check(saw_mem, "found the memory provider BY NAME, knowing only class 0");
    utest_check(saw_self, "the registry lists itself");

    // --- walking past the end is an ERROR, not an empty record --------
    // The distinction an enumerator's exit condition depends on.
    utest_check(sys_query_record(QUERY_MEMINFO, 1, &m, sizeof m) < 0,
          "index 1 of a scalar is refused");
    check_errno(sys_errno(), ERANGE, "past the end says out of range");
    utest_check(sys_query_record(4242, 0, &m, sizeof m) < 0, "an unknown class is refused");
    check_errno(sys_errno(), ENOENT, "an unknown class says no such fact");

    // --- named fields, resolved kernel-side ---------------------------
    unsigned long long v = 0;
    unsigned type = 99;
    utest_check(sys_query_field_get("mem.frame_total", &v, &type) == 0,
          "read a field by qualified name");
    // Compared against the RECORD, not merely nonzero: a field table
    // with two offsets swapped passes any "is it plausible" check.
    utest_check(v == m.frame_total, "the field agrees with the whole record");
    utest_check(type == QUERY_TYPE_U64, "and reports its type");
    utest_check(sys_query_field_get("mem.heap_total_bytes", &v, &type) == 0 &&
          type == QUERY_TYPE_BYTES, "a byte-count field says it is bytes");

    utest_check(sys_query_field_get("providers.anything", &v, &type) < 0,
          "a LIST class has no single value");
    check_errno(sys_errno(), ENOTSUP, "and says so distinctly from 'no such fact'");
    utest_check(sys_query_field_get("mem.nope", &v, &type) < 0, "an unknown field is refused");
    check_errno(sys_errno(), ENOENT, "an unknown field says no such fact");

    // --- the schema ops -----------------------------------------------
    int fields = sys_query_field_count(QUERY_MEMINFO);
    utest_check(fields > 0, "the memory class reports how many fields it has");
    int named = 0;
    for (int i = 0; i < fields; i++) {
        char name[QUERY_FIELD_PATH_MAX];
        unsigned t = 0;
        if (sys_query_field_info(QUERY_MEMINFO, i, name, &t) != 0) break;
        if (strcmp(name, "frame_free") == 0) named = 1;
    }
    utest_check(named, "enumerating the fields finds one by name");

    // --- a fact is LIVE, which is what makes it a fact -----------------
    // Two reads of a counter that must move. Allocating is what moves it,
    // so this asks for memory rather than hoping something else did --
    // a check that merely read twice would pass on a frozen value.
    struct query_meminfo before, after;
    sys_query_record(QUERY_MEMINFO, 0, &before, sizeof before);
    void *big = sys_sbrk(1 << 20);
    if (big != (void *)-1) {
        volatile char *touch = big;
        for (int i = 0; i < (1 << 20); i += 4096) touch[i] = 1; // fault them in
    }
    sys_query_record(QUERY_MEMINFO, 0, &after, sizeof after);
    utest_check(after.frame_free < before.frame_free,
          "free frames FELL after this process touched a megabyte");

    return utest_end();
}
