#ifndef TC_FMT_H
#define TC_FMT_H

#include <stddef.h>

void fmt_unix_time(long long t, char *out, size_t cap);
void fmt_size(long long bytes, char *out, size_t cap);

#endif
