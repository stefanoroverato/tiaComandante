#include "tia_env.h"

#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#define OPENNESS_KEY L"SOFTWARE\\Siemens\\Automation\\Openness\\21.0"

static int dir_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int from_registry(wchar_t *out, size_t cap)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, OPENNESS_KEY L"\\PublicAPI", 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return -1;
    int rc = -1;
    wchar_t ver[128];
    for (DWORD i = 0; rc != 0; i++) {
        DWORD n = sizeof ver / sizeof ver[0];
        if (RegEnumKeyExW(key, i, ver, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        wchar_t sub[256];
        _snwprintf_s(sub, sizeof sub / sizeof sub[0], _TRUNCATE, L"%ls\\net48", ver);
        wchar_t path[MAX_PATH];
        DWORD size = sizeof path;
        if (RegGetValueW(key, sub, L"Siemens.Engineering.Base", RRF_RT_REG_SZ, NULL, path, &size) != ERROR_SUCCESS)
            continue;
        wchar_t *slash = wcsrchr(path, L'\\');
        if (!slash)
            continue;
        *slash = 0;
        if (dir_exists(path)) {
            wcsncpy_s(out, cap, path, _TRUNCATE);
            rc = 0;
        }
    }
    RegCloseKey(key);
    return rc;
}

int tia_find_openness_dir(wchar_t *out, size_t cap)
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"TIACMD_OPENNESS_DIR", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH && dir_exists(buf)) {
        wcsncpy_s(out, cap, buf, _TRUNCATE);
        return 0;
    }
    if (from_registry(out, cap) == 0)
        return 0;
    n = GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t p[MAX_PATH];
        _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%ls\\Siemens\\Automation\\Portal V21\\PublicAPI\\V21\\net48", buf);
        if (dir_exists(p)) {
            wcsncpy_s(out, cap, p, _TRUNCATE);
            return 0;
        }
    }
    return -1;
}

void tia_portal_version(char *out, size_t cap)
{
    char buf[64] = "";
    DWORD size = sizeof buf;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Siemens\\Automation\\Openness\\21.0", "PortalVersion",
                     RRF_RT_REG_SZ, NULL, buf, &size) != ERROR_SUCCESS)
        buf[0] = 0;
    snprintf(out, cap, "%s", buf);
}
