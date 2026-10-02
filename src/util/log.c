#include "log.h"
#include "utf.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static CRITICAL_SECTION g_lock;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static FILE *g_file;
static int g_level = LOG_INFO;
static int g_stderr = 1;

static BOOL CALLBACK init_lock(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

static void ensure_lock(void)
{
    InitOnceExecuteOnce(&g_once, init_lock, NULL, NULL);
}

void log_init(const char *dir)
{
    ensure_lock();
    if (!dir || !*dir)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[1024];
    snprintf(path, sizeof path, "%s\\tiacomandante-%04u%02u%02u.log", dir, st.wYear, st.wMonth, st.wDay);
    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath)
        return;
    EnterCriticalSection(&g_lock);
    if (g_file)
        fclose(g_file);
    g_file = _wfsopen(wpath, L"ab", 0x40 /* _SH_DENYNO */);
    LeaveCriticalSection(&g_lock);
    free(wpath);
}

void log_set_level(int level) { g_level = level; }
void log_set_stderr(int enabled) { g_stderr = enabled; }

void log_write(int level, const char *fmt, ...)
{
    static const char *names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    if (level < g_level)
        return;
    ensure_lock();
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[4200];
    int n = snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] [%lu] %s\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     names[level < 0 ? 0 : level > 3 ? 3 : level], GetCurrentThreadId(), msg);
    if (n < 0)
        return;
    if (n >= (int)sizeof line)
        n = (int)sizeof line - 1;

    EnterCriticalSection(&g_lock);
    if (g_stderr) {
        fwrite(line, 1, (size_t)n, stderr);
        fflush(stderr);
    }
    if (g_file) {
        fwrite(line, 1, (size_t)n, g_file);
        fflush(g_file);
    }
    LeaveCriticalSection(&g_lock);
}

void log_close(void)
{
    ensure_lock();
    EnterCriticalSection(&g_lock);
    if (g_file)
        fclose(g_file);
    g_file = NULL;
    LeaveCriticalSection(&g_lock);
}
