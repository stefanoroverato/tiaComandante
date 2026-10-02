#ifndef TC_STRBUF_H
#define TC_STRBUF_H

#include <stddef.h>

/* Growable, always NUL-terminated UTF-8 buffer. */
typedef struct strbuf {
    char *p;
    size_t len;
    size_t cap;
} strbuf;

void sb_init(strbuf *sb);
void sb_free(strbuf *sb);
void sb_clear(strbuf *sb);
void sb_append(strbuf *sb, const char *s);
void sb_appendn(strbuf *sb, const char *s, size_t n);
void sb_appendc(strbuf *sb, char c);
void sb_printf(strbuf *sb, const char *fmt, ...);
/* Returns the buffer (caller frees) and resets sb. Never NULL. */
char *sb_detach(strbuf *sb);
const char *sb_str(const strbuf *sb);

#endif
