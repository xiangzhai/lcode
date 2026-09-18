#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sb_init(StrBuf *sb, size_t cap)
{
    sb->buf = malloc(cap > 0 ? cap : 1);
    if (!sb->buf)
        return -1;
    sb->buf[0] = '\0';
    sb->len = 0;
    sb->cap = cap > 0 ? cap : 1;
    return 0;
}

static int sb_grow(StrBuf *sb, size_t need)
{
    if (sb->len + need + 1 <= sb->cap)
        return 0;
    size_t ncap = sb->cap ? sb->cap : 1;
    while (sb->len + need + 1 > ncap)
        ncap *= 2;
    char *tmp = realloc(sb->buf, ncap);
    if (!tmp)
        return -1;
    sb->buf = tmp;
    sb->cap = ncap;
    return 0;
}

int sb_addn(StrBuf *sb, const char *data, size_t n)
{
    if (sb_grow(sb, n) != 0)
        return -1;
    memcpy(sb->buf + sb->len, data, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return 0;
}

int sb_add(StrBuf *sb, const char *s)
{
    return sb_addn(sb, s, strlen(s));
}

int sb_addf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (need < 0) {
        va_end(ap);
        return -1;
    }
    if (sb_grow(sb, (size_t)need) != 0) {
        va_end(ap);
        return -1;
    }
    vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    sb->len += (size_t)need;
    return 0;
}

char *sb_steal(StrBuf *sb)
{
    char *b = sb->buf;
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
    return b;
}

void sb_free(StrBuf *sb)
{
    free(sb->buf);
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}
