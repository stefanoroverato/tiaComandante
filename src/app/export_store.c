#include "export_store.h"

#include "cJSON.h"
#include "util/fs.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile LONG g_seq;

static void store_dir(char *out, size_t cap)
{
    fs_join(out, cap, g_cfg.data_dir, "store");
    fs_mkdirs(out);
}

static int valid_id(const char *id)
{
    if (!id || !*id || strlen(id) >= 48)
        return 0;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return 0;
    return 1;
}

static void paths_for(const char *id, char *meta, size_t mcap, char *data, size_t dcap)
{
    char dir[TC_PATH_MAX], name[96];
    store_dir(dir, sizeof dir);
    snprintf(name, sizeof name, "%s.json", id);
    fs_join(meta, mcap, dir, name);
    snprintf(name, sizeof name, "%s.dat", id);
    fs_join(data, dcap, dir, name);
}

static void new_id(char *out, size_t cap)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, cap, "exp-%04u%02u%02u-%02u%02u%02u-%lu-%ld", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, GetCurrentProcessId() % 100000, InterlockedIncrement(&g_seq));
}

static int write_meta(const export_info *e)
{
    char meta[TC_PATH_MAX], data[TC_PATH_MAX];
    paths_for(e->id, meta, sizeof meta, data, sizeof data);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", e->id);
    cJSON_AddStringToObject(o, "tool", e->tool);
    cJSON_AddStringToObject(o, "action", e->action);
    cJSON_AddStringToObject(o, "name", e->name);
    cJSON_AddStringToObject(o, "contentType", e->content_type);
    cJSON_AddNumberToObject(o, "created", (double)e->created);
    cJSON_AddNumberToObject(o, "size", (double)e->size);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    int rc = s ? fs_write_all(meta, s, strlen(s)) : -1;
    cJSON_free(s);
    return rc;
}

static void fill_info(export_info *e, const char *id, const char *tool, const char *action, const char *name,
                      const char *content_type)
{
    memset(e, 0, sizeof *e);
    if (id)
        snprintf(e->id, sizeof e->id, "%s", id);
    else
        new_id(e->id, sizeof e->id);
    snprintf(e->tool, sizeof e->tool, "%s", tool ? tool : "");
    snprintf(e->action, sizeof e->action, "%s", action ? action : "");
    snprintf(e->name, sizeof e->name, "%s", name ? name : e->id);
    snprintf(e->content_type, sizeof e->content_type, "%s", content_type ? content_type : "text/plain");
    e->created = (long long)time(NULL);
    char meta[TC_PATH_MAX];
    paths_for(e->id, meta, sizeof meta, e->path, sizeof e->path);
}

int export_put(const char *tool, const char *action, const char *name, const char *content_type,
               const void *data, size_t len, export_info *out)
{
    export_info e;
    fill_info(&e, NULL, tool, action, name, content_type);
    if (fs_write_all(e.path, data, len) != 0)
        return -1;
    e.size = (long long)len;
    if (write_meta(&e) != 0)
        return -1;
    if (out)
        *out = e;
    return 0;
}

int export_put_file(const char *tool, const char *action, const char *name, const char *content_type,
                    const char *src_path, export_info *out)
{
    export_info e;
    fill_info(&e, NULL, tool, action, name ? name : fs_basename(src_path), content_type);
    if (fs_copy(src_path, e.path, 1) != 0)
        return -1;
    e.size = fs_file_size(e.path);
    if (write_meta(&e) != 0)
        return -1;
    if (out)
        *out = e;
    return 0;
}

static int read_meta(const char *meta_path, export_info *e)
{
    char *text = NULL;
    if (fs_read_all(meta_path, &text, NULL) != 0)
        return -1;
    cJSON *o = cJSON_Parse(text);
    free(text);
    if (!o)
        return -1;
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "id"));
    if (!valid_id(id)) {
        cJSON_Delete(o);
        return -1;
    }
    fill_info(e, id, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "tool")),
              cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "action")),
              cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "name")),
              cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "contentType")));
    e->created = (long long)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "created"));
    e->size = (long long)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "size"));
    cJSON_Delete(o);
    return 0;
}

int export_get(const char *id, export_info *out)
{
    if (!valid_id(id))
        return -1;
    char meta[TC_PATH_MAX], data[TC_PATH_MAX];
    paths_for(id, meta, sizeof meta, data, sizeof data);
    if (read_meta(meta, out) != 0 || !fs_is_file(out->path))
        return -1;
    return 0;
}

typedef struct list_ctx {
    export_info *items;
    int count, cap;
} list_ctx;

static int collect(void *ctx, const char *path, int is_dir, long long size, long long mtime)
{
    (void)size;
    (void)mtime;
    list_ctx *l = ctx;
    if (is_dir)
        return 0;
    export_info e;
    if (read_meta(path, &e) != 0)
        return 0;
    if (l->count == l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        export_info *n = realloc(l->items, (size_t)cap * sizeof *n);
        if (!n)
            return 1;
        l->items = n;
        l->cap = cap;
    }
    l->items[l->count++] = e;
    return 0;
}

static int by_created_desc(const void *a, const void *b)
{
    const export_info *x = a, *y = b;
    if (x->created != y->created)
        return x->created < y->created ? 1 : -1;
    return strcmp(y->id, x->id);
}

static void load_all(list_ctx *l)
{
    char dir[TC_PATH_MAX];
    store_dir(dir, sizeof dir);
    memset(l, 0, sizeof *l);
    fs_walk(dir, "*.json", 0, collect, l);
    if (l->count > 1)
        qsort(l->items, (size_t)l->count, sizeof *l->items, by_created_desc);
}

int export_list(const char *tool_filter, int limit, export_info **list, int *count)
{
    list_ctx l;
    load_all(&l);
    int n = 0;
    for (int i = 0; i < l.count; i++) {
        if (tool_filter && *tool_filter && _stricmp(l.items[i].tool, tool_filter) != 0)
            continue;
        if (limit > 0 && n >= limit)
            break;
        l.items[n++] = l.items[i];
    }
    *list = l.items;
    *count = n;
    return 0;
}

int export_delete(const char *id)
{
    if (!valid_id(id))
        return -1;
    char meta[TC_PATH_MAX], data[TC_PATH_MAX];
    paths_for(id, meta, sizeof meta, data, sizeof data);
    int had = fs_is_file(meta);
    fs_remove(data);
    fs_remove(meta);
    return had ? 0 : -1;
}

int export_clear(double older_than_hours)
{
    list_ctx l;
    load_all(&l);
    long long now = (long long)time(NULL);
    int removed = 0;
    for (int i = 0; i < l.count; i++) {
        if ((double)(now - l.items[i].created) >= older_than_hours * 3600.0 && export_delete(l.items[i].id) == 0)
            removed++;
    }
    free(l.items);
    return removed;
}

long long export_total_size(int *count)
{
    list_ctx l;
    load_all(&l);
    long long total = 0;
    for (int i = 0; i < l.count; i++)
        total += l.items[i].size;
    if (count)
        *count = l.count;
    free(l.items);
    return total;
}
