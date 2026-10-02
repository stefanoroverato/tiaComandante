#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sb_reserve(strbuf *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap)
        return;
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < need)
        cap *= 2;
    char *p = realloc(sb->p, cap);
    if (!p)
        abort();
    sb->p = p;
    sb->cap = cap;
}

void sb_init(strbuf *sb)
{
    sb->p = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_free(strbuf *sb)
{
    free(sb->p);
    sb_init(sb);
}

void sb_clear(strbuf *sb)
{
    sb->len = 0;
    if (sb->p)
        sb->p[0] = 0;
}

void sb_appendn(strbuf *sb, const char *s, size_t n)
{
    sb_reserve(sb, n);
    memcpy(sb->p + sb->len, s, n);
    sb->len += n;
    sb->p[sb->len] = 0;
}

void sb_append(strbuf *sb, const char *s)
{
    if (s)
        sb_appendn(sb, s, strlen(s));
}

void sb_appendc(strbuf *sb, char c)
{
    sb_appendn(sb, &c, 1);
}

void sb_printf(strbuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    sb_reserve(sb, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(sb->p + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
}

char *sb_detach(strbuf *sb)
{
    char *p = sb->p;
    if (!p) {
        p = malloc(1);
        if (!p)
            abort();
        p[0] = 0;
    }
    sb_init(sb);
    return p;
}

const char *sb_str(const strbuf *sb)
{
    return sb->p ? sb->p : "";
}
