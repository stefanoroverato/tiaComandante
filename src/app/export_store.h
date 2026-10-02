/* Export store: results kept on disk under %LOCALAPPDATA%\tiaComandante\store
   and referenced by exportId (replaces TiaCommander's SQLite export DB).
   Entries expire after 24 hours. */
#ifndef TC_EXPORT_STORE_H
#define TC_EXPORT_STORE_H

#include "config.h"

#include <stddef.h>

typedef struct export_info {
    char id[48];
    char tool[48];
    char action[64];
    char name[256];         /* suggested file name */
    char content_type[64];  /* e.g. application/xml, text/csv */
    long long created;      /* unix seconds */
    long long size;
    char path[TC_PATH_MAX]; /* data file */
} export_info;

int export_put(const char *tool, const char *action, const char *name, const char *content_type,
               const void *data, size_t len, export_info *out);
int export_put_file(const char *tool, const char *action, const char *name, const char *content_type,
                    const char *src_path, export_info *out);
int export_get(const char *id, export_info *out);
/* Newest first; caller frees *list. */
int export_list(const char *tool_filter, int limit, export_info **list, int *count);
int export_delete(const char *id);
/* Deletes entries older than the given age; returns the number removed. */
int export_clear(double older_than_hours);
long long export_total_size(int *count);

#endif
