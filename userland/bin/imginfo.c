// imginfo -- what an image file is, and what one pixel of it decodes to.
//
// The header half is what `file` or `identify` gives you: format,
// dimensions, and how the picture is actually stored, which for a JPEG
// is the part that decides whether this build can show it at all.
//
// The pixel half exists for TESTING, and it is the reason this command
// is worth having rather than being a menu item in the viewer. A decoder
// is verified by comparing numbers against another implementation's, and
// a screenshot cannot do that: `imginfo -p 4,4 photo.jpg` prints one
// pixel's value as text, so tools/vm.py can assert on it from the host
// with no framebuffer, no compositor and no pixels involved.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/uimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <kerrno.h>

static const char *why(int rc) {
    // -ENOTSUP is not a broken file and must not read like one; see
    // uimg.h on why the two codes are kept apart.
    if (rc == -ENOTSUP) return "unsupported";
    if (rc == -ENOMEM)  return "out of memory";
    if (rc == -ENOENT)  return "not found";
    return "invalid";
}

static void report(const char *path, int rc) {
    char buf[160];
    snprintf(buf, sizeof buf, "imginfo: %s: %s: %s\n", path, why(rc),
             uimg_last_error());
    sys_print(buf);
}

// "4,4" -> 4, 4. Returns 0 on anything that is not two numbers.
static int parse_xy(const char *s, int *x, int *y) {
    char *end;
    long a = strtol(s, &end, 10);
    if (end == s || *end != ',') return 0;
    const char *s2 = end + 1;
    long b = strtol(s2, &end, 10);
    if (end == s2 || *end != '\0') return 0;
    if (a < 0 || b < 0) return 0;
    *x = (int)a;
    *y = (int)b;
    return 1;
}

int main(int argc, char **argv) {
    const char *pixel = NULL;
    int decode = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (argv[i][1] == 'p' && argv[i][2] == '\0' && i + 1 < argc) {
            pixel = argv[++i];
        } else if (argv[i][1] == 'd' && argv[i][2] == '\0') {
            decode = 1;
        } else {
            cmd_usage("imginfo [-d] [-p x,y] <path>");
            return 1;
        }
    }
    if (i != argc - 1) {
        cmd_usage("imginfo [-d] [-p x,y] <path>");
        return 1;
    }
    const char *path = argv[i];

    struct uimg_info info;
    int rc = uimg_load_info(path, &info);
    if (rc < 0) {
        report(path, rc);
        return 1;
    }

    char buf[192];
    snprintf(buf, sizeof buf, "%s: %s %dx%d, %d component%s (%s)\n",
             path, info.format, info.w, info.h, info.components,
             info.components == 1 ? "" : "s", info.detail);
    sys_print(buf);

    if (!pixel && !decode) return 0;

    struct uimg im;
    unsigned long long t0 = sys_monotonic_ns();
    rc = uimg_load(path, &im);
    unsigned long long ms = (sys_monotonic_ns() - t0) / 1000000ull;
    if (rc < 0) {
        report(path, rc);
        return 1;
    }
    // The time is printed because it is the number that decides things:
    // every automated boot here is TCG, where a full-screen photograph
    // is the desktop's slowest startup step, and "how long does this
    // wallpaper cost" is otherwise a guess.
    snprintf(buf, sizeof buf, "decoded: %dx%d, %lu bytes of pixels, %llu ms\n",
             im.w, im.h, (unsigned long)((size_t)im.w * im.h * 4), ms);
    sys_print(buf);

    if (pixel) {
        int x, y;
        if (!parse_xy(pixel, &x, &y)) {
            cmd_usage("imginfo [-d] [-p x,y] <path>");
            uimg_free(&im);
            return 1;
        }
        if (x >= im.w || y >= im.h) {
            sys_print("imginfo: that pixel is outside the image\n");
            uimg_free(&im);
            return 1;
        }
        uint32_t p = im.px[(size_t)y * im.w + x];
        snprintf(buf, sizeof buf, "pixel %d,%d: %02x%02x%02x\n", x, y,
                 (unsigned)((p >> 16) & 0xFF), (unsigned)((p >> 8) & 0xFF),
                 (unsigned)(p & 0xFF));
        sys_print(buf);
    }

    uimg_free(&im);
    return 0;
}
