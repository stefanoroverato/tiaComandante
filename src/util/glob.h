#ifndef TC_GLOB_H
#define TC_GLOB_H

/* Glob match: '*' any sequence, '?' one character. Returns 1 on match. */
int glob_match(const char *pattern, const char *text, int case_sensitive);
/* 1 if the pattern contains wildcards. */
int glob_has_wildcards(const char *pattern);

#endif
