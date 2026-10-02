#include "glob.h"

#include <ctype.h>
#include <string.h>

static int eq(char a, char b, int cs)
{
    return cs ? a == b : tolower((unsigned char)a) == tolower((unsigned char)b);
}

int glob_match(const char *p, const char *t, int cs)
{
    const char *star = NULL, *back = NULL;
    while (*t) {
        if (*p == '*') {
            star = p++;
            back = t;
        } else if (*p == '?' || (*p && eq(*p, *t, cs))) {
            p++;
            t++;
        } else if (star) {
            p = star + 1;
            t = ++back;
        } else {
            return 0;
        }
    }
    while (*p == '*')
        p++;
    return *p == 0;
}

int glob_has_wildcards(const char *pattern)
{
    return strpbrk(pattern, "*?") != NULL;
}
