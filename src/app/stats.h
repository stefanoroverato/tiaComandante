/* In-memory call statistics and recent errors (admin get_stats / get_recent_errors). */
#ifndef TC_STATS_H
#define TC_STATS_H

#include "util/strbuf.h"

void stats_init(void);
void stats_record(const char *tool, const char *action, int ok, double ms, const char *error_text, const char *context);
void stats_report(strbuf *out, int top_n);
void stats_recent_errors(strbuf *out, int count);
long long stats_uptime_seconds(void);

#endif
