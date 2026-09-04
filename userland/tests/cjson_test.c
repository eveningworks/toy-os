// STAGE 6 OF THE C LIBRARY: a program nobody working on this repo wrote
// (docs/libc-design.md).
//
// THIS FILE IS OURS; cJSON IS NOT. userland/ports/cjson/ is upstream's
// source, byte for byte -- and that is the entire point. Every other
// test under userland/tests/ was written by somebody who knew what this
// C library supported, so none of them can find a gap: they were
// written around one. cJSON was written years before this OS existed
// and asks for whatever C says exists.
//
// WHAT IT ACTUALLY EXERCISES, which is why this library and not a
// smaller one: malloc/realloc on every node, strtod on every number,
// sprintf on every number printed back, sscanf to check the round trip,
// fabs/isnan/isinf, and most of <string.h>. A pass here is a statement
// about Stages 2-5 together.
//
// THE ASSERTION IS A ROUND TRIP, not "it printed something". Parse,
// re-serialise, parse the result again, and compare the VALUES -- a
// broken strtod that read 3.25 as 3.0 would still print a valid JSON
// document, and would fail this.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "ports/cjson/cJSON.h"

#include "lib/utest.h"

static const char *DOC =
    "{"
      "\"name\":\"toy-os\","
      "\"version\":3,"
      "\"ratio\":3.25,"
      "\"tiny\":0.0001,"
      "\"stable\":true,"
      "\"missing\":null,"
      "\"stages\":[0,1,2,3,4,5],"
      "\"nested\":{\"deep\":{\"value\":42}},"
      "\"escaped\":\"a\\\"b\\\\c\\td\""
    "}";

int main(void) {
    utest_begin("cjson_test", "a program nobody here wrote, on toy-os's libc", 0);
    utest_notef("cJSON version %s", cJSON_Version());

    // --- parse --------------------------------------------------------
    cJSON *root = cJSON_Parse(DOC);
    if (!root) {
        const char *err = cJSON_GetErrorPtr();
        utest_checkf(0, "cJSON_Parse returned NULL at: %s", err ? err : "(no position)");
        return utest_end();
    }
    utest_check(1, "cJSON_Parse accepted the document");

    cJSON *name = cJSON_GetObjectItem(root, "name");
    utest_check(name && cJSON_IsString(name) && strcmp(name->valuestring, "toy-os") == 0,
          "a string value survived parsing");
    cJSON *ver = cJSON_GetObjectItem(root, "version");
    utest_check(ver && cJSON_IsNumber(ver) && ver->valueint == 3, "an integer");
    cJSON *ratio = cJSON_GetObjectItem(root, "ratio");
    // strtod's job. A parser that read this as 3.0 still produces valid
    // JSON on the way out, which is why the value is checked and not
    // just the shape.
    utest_check(ratio && cJSON_IsNumber(ratio) && ratio->valuedouble == 3.25,
          "a fraction, exactly (strtod)");
    cJSON *tiny = cJSON_GetObjectItem(root, "tiny");
    utest_check(tiny && fabs(tiny->valuedouble - 0.0001) < 1e-12, "and a small one");
    utest_check(cJSON_IsTrue(cJSON_GetObjectItem(root, "stable")), "true");
    utest_check(cJSON_IsNull(cJSON_GetObjectItem(root, "missing")), "null");

    cJSON *stages = cJSON_GetObjectItem(root, "stages");
    utest_check(stages && cJSON_IsArray(stages) && cJSON_GetArraySize(stages) == 6,
          "an array of six");
    int sum = 0;
    cJSON *el;
    cJSON_ArrayForEach(el, stages) sum += el->valueint;
    utest_check(sum == 15, "and every element is readable");

    cJSON *deep = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "nested"), "deep");
    utest_check(deep && cJSON_GetObjectItem(deep, "value")->valueint == 42,
          "a nested object two levels down");
    cJSON *esc = cJSON_GetObjectItem(root, "escaped");
    utest_check(esc && strcmp(esc->valuestring, "a\"b\\c\td") == 0,
          "backslash escapes were decoded");

    // --- serialise, and round trip ------------------------------------
    char *out = cJSON_PrintUnformatted(root);
    utest_check(out != 0, "cJSON_Print produced a document");
    if (out) {
        cJSON *again = cJSON_Parse(out);
        utest_check(again != 0, "which parses back");
        if (again) {
            // THE LOAD-BEARING CHECK. sprintf wrote these numbers and
            // strtod read them; a %g that lost digits or a strtod that
            // rounded differently shows up here and nowhere else.
            utest_check(cJSON_GetObjectItem(again, "ratio")->valuedouble == 3.25,
                  "and the fraction survived the ROUND TRIP");
            utest_check(cJSON_GetObjectItem(again, "version")->valueint == 3,
                  "and the integer did");
            utest_check(strcmp(cJSON_GetObjectItem(again, "name")->valuestring, "toy-os") == 0,
                  "and the string did");
            utest_check(strcmp(cJSON_GetObjectItem(again, "escaped")->valuestring,
                         "a\"b\\c\td") == 0,
                  "and the escapes were re-encoded correctly");
            utest_check(cJSON_GetArraySize(cJSON_GetObjectItem(again, "stages")) == 6,
                  "and the array kept its length");
            cJSON_Delete(again);
        }
        free(out);
    }

    // --- build a document from scratch --------------------------------
    cJSON *made = cJSON_CreateObject();
    cJSON_AddStringToObject(made, "built", "here");
    cJSON_AddNumberToObject(made, "pi", 3.14159);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 1; i <= 3; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(i * 1.5));
    cJSON_AddItemToObject(made, "halves", arr);
    char *made_txt = cJSON_PrintUnformatted(made);
    utest_check(made_txt != 0, "a document built through the API serialises");
    if (made_txt) {
        cJSON *reread = cJSON_Parse(made_txt);
        utest_check(reread != 0, "and parses back");
        if (reread) {
            utest_check(fabs(cJSON_GetObjectItem(reread, "pi")->valuedouble - 3.14159) < 1e-9,
                  "with pi intact");
            utest_check(cJSON_GetArrayItem(cJSON_GetObjectItem(reread, "halves"), 2)->valuedouble == 4.5,
                  "and 3 * 1.5 == 4.5 after a text round trip");
            cJSON_Delete(reread);
        }
        free(made_txt);
    }
    cJSON_Delete(made);

    // --- it must REJECT bad input -------------------------------------
    // A parser that accepted everything would pass every check above.
    utest_check(cJSON_Parse("{\"a\":}") == 0, "malformed JSON is rejected");
    utest_check(cJSON_Parse("[1,2") == 0, "and so is a truncated array");

    cJSON_Delete(root);

    return utest_end();
}
