#include "fs.h"
#include "utf.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t zmin(size_t a, size_t b) { return a < b ? a : b; }

static long long filetime_to_unix(FILETIME ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000000ULL) - 11644473600LL;
}

int fs_app_dir(int which, char *out, size_t cap)
{
    PWSTR base = NULL;
    const KNOWNFOLDERID *id = which == FS_LOCAL ? &FOLDERID_LocalAppData : &FOLDERID_RoamingAppData;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, NULL, &base)))
        return -1;
    char *b = wide_to_utf8(base);
    CoTaskMemFree(base);
    if (!b)
        return -1;
    snprintf(out, cap, "%s\\tiaComandante", b);
    free(b);
    return fs_mkdirs(out);
}

int fs_exe_dir(char *out, size_t cap)
{
    wchar_t w[MAX_PATH];
    GetModuleFileNameW(NULL, w, MAX_PATH);
    wchar_t *slash = wcsrchr(w, L'\\');
    if (slash)
        slash[1] = 0;
    char *s = wide_to_utf8(w);
    if (!s)
        return -1;
    snprintf(out, cap, "%s", s);
    free(s);
    return 0;
}

static DWORD attrs(const char *path)
{
    wchar_t *w = utf8_to_wide(path);
    if (!w)
        return INVALID_FILE_ATTRIBUTES;
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a;
}

int fs_is_dir(const char *path)
{
    DWORD a = attrs(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int fs_is_file(const char *path)
{
    DWORD a = attrs(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int fs_mkdirs(const char *path)
{
    if (!path || !*path)
        return -1;
    if (fs_is_dir(path))
        return 0;
    wchar_t *w = utf8_to_wide(path);
    if (!w)
        return -1;
    int rc = SHCreateDirectoryExW(NULL, w, NULL);
    free(w);
    return (rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS) ? 0 : -1;
}

int fs_remove(const char *path)
{
    wchar_t *w = utf8_to_wide(path);
    if (!w)
        return -1;
    BOOL ok = DeleteFileW(w);
    free(w);
    return ok ? 0 : -1;
}

int fs_remove_tree(const char *path)
{
    wchar_t *w = utf8_to_wide(path);
    if (!w)
        return -1;
    size_t n = wcslen(w);
    wchar_t *dbl = calloc(n + 2, sizeof(wchar_t)); /* SHFileOperation needs a double NUL */
    if (!dbl) {
        free(w);
        return -1;
    }
    memcpy(dbl, w, n * sizeof(wchar_t));
    free(w);
    SHFILEOPSTRUCTW op = { 0 };
    op.wFunc = FO_DELETE;
    op.pFrom = dbl;
    op.fFlags = FOF_NO_UI;
    int rc = SHFileOperationW(&op);
    free(dbl);
    return rc == 0 ? 0 : -1;
}

static int stat_file(const char *path, WIN32_FILE_ATTRIBUTE_DATA *d)
{
    wchar_t *w = utf8_to_wide(path);
    if (!w)
        return -1;
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, d);
    free(w);
    return ok ? 0 : -1;
}

long long fs_file_size(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (stat_file(path, &d) != 0)
        return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

long long fs_file_mtime(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (stat_file(path, &d) != 0)
        return -1;
    return filetime_to_unix(d.ftLastWriteTime);
}

int fs_read_all(const char *path, char **data, size_t *len)
{
    *data = NULL;
    if (len)
        *len = 0;
    wchar_t *w = utf8_to_wide(path);
    if (!w)
        return -1;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 512LL * 1024 * 1024) {
        CloseHandle(h);
        return -1;
    }
    char *buf = malloc((size_t)size.QuadPart + 1);
    if (!buf) {
        CloseHandle(h);
        return -1;
    }
    size_t total = 0;
    while (total < (size_t)size.QuadPart) {
        DWORD got = 0;
        DWORD want = (DWORD)zmin((size_t)size.QuadPart - total, (size_t)1 << 30);
        if (!ReadFile(h, buf + total, want, &got, NULL) || got == 0)
            break;
        total += got;
    }
    CloseHandle(h);
    buf[total] = 0;
    *data = buf;
    if (len)
        *len = total;
    return 0;
}

int fs_write_all(const char *path, const void *data, size_t len)
{
    wchar_t *w = utf8_to_wide(path);
    if (!w)
        return -1;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    const char *p = data;
    size_t left = len;
    while (left > 0) {
        DWORD put = 0;
        DWORD chunk = (DWORD)zmin(left, (size_t)1 << 30);
        if (!WriteFile(h, p, chunk, &put, NULL) || put == 0) {
            CloseHandle(h);
            return -1;
        }
        p += put;
        left -= put;
    }
    CloseHandle(h);
    return 0;
}

int fs_copy(const char *from, const char *to, int overwrite)
{
    wchar_t *a = utf8_to_wide(from), *b = utf8_to_wide(to);
    BOOL ok = a && b && CopyFileW(a, b, !overwrite);
    free(a);
    free(b);
    return ok ? 0 : -1;
}

int fs_move(const char *from, const char *to)
{
    wchar_t *a = utf8_to_wide(from), *b = utf8_to_wide(to);
    BOOL ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    free(a);
    free(b);
    return ok ? 0 : -1;
}

void fs_join(char *out, size_t cap, const char *a, const char *b)
{
    size_t n = strlen(a);
    const char *sep = (n > 0 && (a[n - 1] == '\\' || a[n - 1] == '/')) ? "" : "\\";
    snprintf(out, cap, "%s%s%s", a, sep, b);
}

const char *fs_basename(const char *path)
{
    const char *p = path, *s;
    for (s = path; *s; s++)
        if (*s == '\\' || *s == '/')
            p = s + 1;
    return p;
}

static volatile LONG g_temp_counter;

static int temp_root(char *out, size_t cap)
{
    wchar_t w[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, w))
        return -1;
    char *t = wide_to_utf8(w);
    if (!t)
        return -1;
    snprintf(out, cap, "%stiaComandante", t);
    free(t);
    return fs_mkdirs(out);
}

int fs_temp_path(const char *prefix, const char *ext, char *out, size_t cap)
{
    char root[MAX_PATH * 2];
    if (temp_root(root, sizeof root) != 0)
        return -1;
    for (int tries = 0; tries < 1000; tries++) {
        snprintf(out, cap, "%s\\%s-%lu-%ld%s", root, prefix, GetCurrentProcessId(),
                 InterlockedIncrement(&g_temp_counter), ext ? ext : "");
        if (!fs_is_file(out) && !fs_is_dir(out))
            return 0;
    }
    return -1;
}

int fs_temp_dir(const char *prefix, char *out, size_t cap)
{
    if (fs_temp_path(prefix, "", out, cap) != 0)
        return -1;
    return fs_mkdirs(out);
}

int fs_full_path(const char *in, char *out, size_t cap)
{
    wchar_t *w = utf8_to_wide(in);
    if (!w)
        return -1;
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(w, MAX_PATH * 4, full, NULL);
    free(w);
    if (n == 0 || n >= MAX_PATH * 4)
        return -1;
    char *s = wide_to_utf8(full);
    if (!s)
        return -1;
    snprintf(out, cap, "%s", s);
    free(s);
    return 0;
}

static int walk(const char *dir, const wchar_t *wpattern, int depth, fs_walk_fn cb, void *ctx)
{
    char spec[MAX_PATH * 4];
    fs_join(spec, sizeof spec, dir, "*");
    wchar_t *wspec = utf8_to_wide(spec);
    if (!wspec)
        return 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wspec, &fd);
    free(wspec);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int stop = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        char *name = wide_to_utf8(fd.cFileName);
        if (!name)
            continue;
        char full[MAX_PATH * 4];
        fs_join(full, sizeof full, dir, name);
        free(name);
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        long long size = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        long long mtime = filetime_to_unix(fd.ftLastWriteTime);
        if (is_dir) {
            if (!wpattern && (stop = cb(ctx, full, 1, 0, mtime)) != 0)
                break;
            if (depth > 0 && (stop = walk(full, wpattern, depth - 1, cb, ctx)) != 0)
                break;
        } else if (!wpattern || PathMatchSpecW(fd.cFileName, wpattern)) {
            if ((stop = cb(ctx, full, 0, size, mtime)) != 0)
                break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return stop;
}

int fs_walk(const char *dir, const char *pattern, int depth, fs_walk_fn cb, void *ctx)
{
    wchar_t *wp = pattern ? utf8_to_wide(pattern) : NULL;
    int rc = walk(dir, wp, depth, cb, ctx);
    free(wp);
    return rc;
}
