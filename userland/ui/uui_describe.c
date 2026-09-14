// uui_describe.c -- the four line shapes of ui/uui_describe.h.
//
// One buffer, one snprintf each: a line that does not fit is DROPPED
// rather than truncated, since a rect with its last number cut off is
// a wrong rect, not a shorter one.
#include "ui/uui_describe.h"
#include <stdio.h>

#define LINE_MAX 128

static void emit(const struct uui_describe *d, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void emit(const struct uui_describe *d, const char *fmt, ...) {
    char line[LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof line) return;
    d->line(d->ctx, line);
}

void uui_describe_rect(const struct uui_describe *d, const char *part,
                       int x, int y, int w, int h) {
    emit(d, "%s: layout %s.%s %d %d %d %d\n", d->prefix, d->name, part, x, y, w, h);
}

void uui_describe_rect_i(const struct uui_describe *d, const char *part, int i,
                         int x, int y, int w, int h) {
    emit(d, "%s: layout %s.%s %d %d %d %d %d\n", d->prefix, d->name, part, i, x, y, w, h);
}

void uui_describe_rect_ij(const struct uui_describe *d, const char *part, int i, int j,
                          int x, int y, int w, int h) {
    emit(d, "%s: layout %s.%s %d %d %d %d %d %d\n", d->prefix, d->name, part, i, j, x, y, w, h);
}

void uui_describe_int(const struct uui_describe *d, const char *part, int v) {
    emit(d, "%s: layout %s.%s %d\n", d->prefix, d->name, part, v);
}

void uui_describe_str(const struct uui_describe *d, const char *part, const char *v) {
    emit(d, "%s: layout %s.%s %s\n", d->prefix, d->name, part, v ? v : "");
}
