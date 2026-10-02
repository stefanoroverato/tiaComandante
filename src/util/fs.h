#ifndef TC_FS_H
#define TC_FS_H

#include <stddef.h>

/* All paths are UTF-8. */

enum { FS_ROAMING = 0, FS_LOCAL = 1 };

/* %APPDATA%\tiaComandante or %LOCALAPPDATA%\tiaComandante (created). */
int fs_app_dir(int which, char *out, size_t cap);
int fs_exe_dir(char *out, size_t cap); /* with trailing backslash */

int fs_is_dir(const char *path);
int fs_is_file(const char *path);
int fs_mkdirs(const char *path);
int fs_remove(const char *path);
int fs_remove_tree(const char *path);
long long fs_file_size(const char *path);
long long fs_file_mtime(const char *path); /* unix seconds, -1 on error */
int fs_read_all(const char *path, char **data, size_t *len); /* malloc'ed, NUL-terminated */
int fs_write_all(const char *path, const void *data, size_t len);
int fs_copy(const char *from, const char *to, int overwrite);
int fs_move(const char *from, const char *to);
void fs_join(char *out, size_t cap, const char *a, const char *b);
const char *fs_basename(const char *path);
/* Unique file path %TEMP%\tiaComandante\<prefix>-<pid>-<n><ext> (directory created). */
int fs_temp_path(const char *prefix, const char *ext, char *out, size_t cap);
/* Unique empty directory under %TEMP%\tiaComandante. */
int fs_temp_dir(const char *prefix, char *out, size_t cap);
int fs_full_path(const char *in, char *out, size_t cap);

typedef int (*fs_walk_fn)(void *ctx, const char *path, int is_dir, long long size, long long mtime);
/* Walks dir (depth 0 = only dir itself); pattern like "*.ap21" applies to files. Stops when cb returns non-zero. */
int fs_walk(const char *dir, const char *pattern, int depth, fs_walk_fn cb, void *ctx);

#endif
