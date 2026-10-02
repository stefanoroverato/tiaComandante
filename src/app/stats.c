#include "stats.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 512
#define MAX_ERRORS 50

typedef struct stat_entry {
    char key[128];
    long calls, errors;
    double total_ms, max_ms;
} stat_entry;

typedef struct err_entry {
    SYSTEMTIME when;
    char key[128];
    char context[256];
    char text[768];
} err_entry;

static SRWLOCK g_lock = SRWLOCK_INIT;
static stat_entry g_stats[MAX_KEYS];
static int g_nstats;
static err_entry g_errors[MAX_ERRORS];
static int g_err_next, g_err_count;
static ULONGLONG g_start;

void stats_init(void)
{
    g_start = GetTickCount64();
}

long long stats_uptime_seconds(void)
{
    return (long long)((GetTickCount64() - g_start) / 1000ULL);
}

void stats_record(const char *tool, const char *action, int ok, double ms, const char *error_text, const char *context)
{
    char key[128];
    snprintf(key, sizeof key, "%s%s%s", tool, action && *action ? "." : "", action ? action : "");
    AcquireSRWLockExclusive(&g_lock);
    stat_entry *e = NULL;
    for (int i = 0; i < g_nstats; i++) {
        if (strcmp(g_stats[i].key, key) == 0) {
            e = &g_stats[i];
            break;
        }
    }
    if (!e && g_nstats < MAX_KEYS) {
        e = &g_stats[g_nstats++];
        memset(e, 0, sizeof *e);
        snprintf(e->key, sizeof e->key, "%s", key);
    }
    if (e) {
        e->calls++;
        if (!ok)
            e->errors++;
        e->total_ms += ms;
        if (ms > e->max_ms)
            e->max_ms = ms;
    }
    if (!ok) {
        err_entry *r = &g_errors[g_err_next];
        GetLocalTime(&r->when);
        snprintf(r->key, sizeof r->key, "%s", key);
        snprintf(r->context, sizeof r->context, "%s", context ? context : "");
        snprintf(r->text, sizeof r->text, "%s", error_text ? error_text : "");
        g_err_next = (g_err_next + 1) % MAX_ERRORS;
        if (g_err_count < MAX_ERRORS)
            g_err_count++;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

static int by_calls(const void *a, const void *b)
{
    const stat_entry *x = a, *y = b;
    return (y->calls > x->calls) - (y->calls < x->calls);
}

void stats_report(strbuf *out, int top_n)
{
    AcquireSRWLockShared(&g_lock);
    int n = g_nstats;
    stat_entry *copy = malloc(sizeof(stat_entry) * (size_t)(n ? n : 1));
    if (copy)
        memcpy(copy, g_stats, sizeof(stat_entry) * (size_t)n);
    ReleaseSRWLockShared(&g_lock);
    if (!copy)
        return;
    qsort(copy, (size_t)n, sizeof *copy, by_calls);
    sb_printf(out, "Call statistics since start (uptime %llds), %d tool/action pairs:\n", stats_uptime_seconds(), n);
    for (int i = 0; i < n && (top_n <= 0 || i < top_n); i++) {
        sb_printf(out, "%s  [calls=%ld, errors=%ld, avg_ms=%.0f, max_ms=%.0f]\n", copy[i].key, copy[i].calls,
                  copy[i].errors, copy[i].total_ms / (double)copy[i].calls, copy[i].max_ms);
    }
    free(copy);
}

void stats_recent_errors(strbuf *out, int count)
{
    AcquireSRWLockShared(&g_lock);
    int n = g_err_count < count ? g_err_count : count;
    sb_printf(out, "Last %d failed call(s):\n", n);
    for (int i = 0; i < n; i++) {
        int idx = (g_err_next - 1 - i + MAX_ERRORS) % MAX_ERRORS;
        const err_entry *r = &g_errors[idx];
        sb_printf(out, "%04u-%02u-%02u %02u:%02u:%02u  %s%s%s%s\n    %s\n", r->when.wYear, r->when.wMonth,
                  r->when.wDay, r->when.wHour, r->when.wMinute, r->when.wSecond, r->key, r->context[0] ? "  [" : "",
                  r->context, r->context[0] ? "]" : "", r->text);
    }
    ReleaseSRWLockShared(&g_lock);
}
