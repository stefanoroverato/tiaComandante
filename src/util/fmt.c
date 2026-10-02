#include "fmt.h"

#include <stdio.h>
#include <time.h>

void fmt_unix_time(long long t, char *out, size_t cap)
{
    struct tm tm;
    time_t tt = (time_t)t;
    if (t <= 0 || localtime_s(&tm, &tt) != 0) {
        snprintf(out, cap, "?");
        return;
    }
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

void fmt_size(long long bytes, char *out, size_t cap)
{
    if (bytes < 0)
        snprintf(out, cap, "?");
    else if (bytes < 1024)
        snprintf(out, cap, "%lld B", bytes);
    else if (bytes < 1024 * 1024)
        snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024)
        snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else
        snprintf(out, cap, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}
