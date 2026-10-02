/* tiacomandante.exe - minimal, rarely changing host.
 *
 * TIA Portal's Openness AllowList identifies a client by the SHA-256 hash of
 * the process executable. Keeping the executable stable and loading all
 * logic from tiacomandante-core.dll means the user approves access once
 * instead of after every rebuild. Do not add logic here.
 */
#include <windows.h>
#include <stdio.h>

typedef int (*tc_main_fn)(void);

int main(void)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return 3;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash || (size_t)(slash - path) + 32 >= MAX_PATH)
        return 3;
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - path), L"tiacomandante-core.dll");

    HMODULE core = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!core) {
        fprintf(stderr, "tiacomandante: cannot load %ls (error %lu)\n", path, GetLastError());
        return 3;
    }
    tc_main_fn entry = (tc_main_fn)(void *)GetProcAddress(core, "tc_main");
    if (!entry) {
        fprintf(stderr, "tiacomandante: tc_main not exported by the core library\n");
        return 3;
    }
    return entry();
}
