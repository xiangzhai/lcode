#ifndef LCODE_STRBUF_H
#define LCODE_STRBUF_H

#include <stddef.h>

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} StrBuf;

int sb_init(StrBuf *sb, size_t cap);
int sb_addn(StrBuf *sb, const char *data, size_t n);
int sb_add(StrBuf *sb, const char *s);
int sb_addf(StrBuf *sb, const char *fmt, ...);
char *sb_steal(StrBuf *sb);
void sb_free(StrBuf *sb);

#endif
