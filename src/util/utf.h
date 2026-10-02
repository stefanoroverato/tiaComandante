#ifndef TC_UTF_H
#define TC_UTF_H

#include <wchar.h>

/* Both return malloc'ed strings (NULL if the input is NULL or invalid). */
wchar_t *utf8_to_wide(const char *s);
char *wide_to_utf8(const wchar_t *w);

/* strdup that tolerates NULL. */
char *tc_strdup(const char *s);

#endif
