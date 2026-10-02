/* folders: project-tree groups of the PLC software containers. */
#include "tools.h"

#include "tia/tia_dyn.h"
#include "tia/tia_sw.h"
#include "util/glob.h"
#include "util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int get_container(tool_ctx *c, sw_container *cont)
{
    const char *name = arg_s(c, "container");
    if (!name || !*name)
        name = "program_blocks";
    if (sw_container_parse(name, cont) != 0)
        return fail(c, "unknown container '%s': use program_blocks, tag_tables, udts, watch_tables, external_sources or "
                       "technology_objects",
                    name);
    return 0;
}

/* ---- tree ---------------------------------------------------------------------- */

typedef struct tree_ctx {
    tool_ctx *c;
    sw_container cont;
    int nested, include_items, max_depth, limit;
    const char *type_filter;
    int lines, folders, items;
    int truncated;
} tree_ctx;

static int count_items(th group, sw_container cont, cJSON **lists, const char *attrs)
{
    static const char *props[SWC_COUNT][2] = {
        { "Blocks", NULL }, { "TagTables", NULL }, { "Types", NULL }, { "WatchTables", "ForceTables" },
        { "ExternalSources", NULL }, { "TechnologicalObjects", NULL },
    };
    int n = 0;
    for (int k = 0; k < 2; k++) {
        lists[k] = NULL;
        if (!props[cont][k])
            continue;
        th coll = td_get_h(group, props[cont][k]);
        lists[k] = coll ? td_enum(coll, attrs, -1) : NULL;
        n += cJSON_GetArraySize(lists[k]);
    }
    return n;
}

static void tree_walk(tree_ctx *t, th group, const char *path, int depth)
{
    if (t->lines >= t->limit) {
        t->truncated = 1;
        return;
    }
    cJSON *lists[2];
    int nitems = count_items(group, t->cont, lists, item_attrs(t->cont));
    cJSON *groups = td_enum(td_get_h(group, "Groups"), "Name", -1);
    int nfolders = cJSON_GetArraySize(groups);
    const char *leaf = strrchr(path, '/');
    leaf = leaf ? leaf + 1 : path;
    if (t->nested)
        out(t->c, "%*s%s/  [folders=%d, items=%d]\n", depth * 2, "", *path ? leaf : sw_container_name(t->cont), nfolders, nitems);
    else
        out(t->c, "%s/  [folders=%d, items=%d]\n", *path ? path : "", nfolders, nitems);
    t->lines++;
    t->folders++;
    if (t->include_items) {
        for (int k = 0; k < 2; k++) {
            const cJSON *it;
            cJSON_ArrayForEach(it, lists[k])
            {
                if (t->type_filter && *t->type_filter && _stricmp(short_type(it), t->type_filter) != 0)
                    continue;
                if (t->lines >= t->limit) {
                    t->truncated = 1;
                    break;
                }
                out(t->c, "%*s", t->nested ? depth * 2 + 2 : 0, "");
                print_item(t->c, t->cont, it, t->nested ? "" : path);
                t->lines++;
                t->items++;
            }
        }
    }
    cJSON_Delete(lists[0]);
    cJSON_Delete(lists[1]);
    if (t->max_depth <= 0 || depth + 1 < t->max_depth) {
        const cJSON *g;
        cJSON_ArrayForEach(g, groups)
        {
            char sub[1024];
            snprintf(sub, sizeof sub, "%s%s%s", path, *path ? "/" : "", tdi_s(g, "Name") ? tdi_s(g, "Name") : "?");
            tree_walk(t, tdv_h(g), sub, depth + 1);
        }
    }
    cJSON_Delete(groups);
}

static int a_get_tree(tool_ctx *c)
{
    nav_plc plc;
    sw_container cont;
    if (sw_plc(c, &plc) != 0 || get_container(c, &cont) != 0)
        return -1;
    const char *path = arg_s(c, "path");
    th start = sw_folder(c, plc.software, cont, path, 0, 0);
    if (!start)
        return -1;
    tree_ctx t = { c, cont, arg_b(c, "nested", 1), arg_b(c, "includeItems", 0), (int)arg_i(c, "depth", 0),
                   (int)arg_i(c, "limit", 1000), arg_s(c, "typeFilter"), 0, 0, 0, 0 };
    if (t.limit <= 0)
        t.limit = 1000;
    char norm[1024] = "";
    if (path) {
        snprintf(norm, sizeof norm, "%s", path);
        for (char *p = norm; *p; p++)
            if (*p == '\\')
                *p = '/';
        while (*norm == '/')
            memmove(norm, norm + 1, strlen(norm));
        size_t n = strlen(norm);
        while (n > 0 && norm[n - 1] == '/')
            norm[--n] = 0;
    }
    tree_walk(&t, start, norm, 0);
    if (t.truncated)
        out(c, "NOTICE: output truncated at %d lines (raise limit or narrow path/depth).\n", t.limit);
    out(c, "%d folder(s)%s", t.folders, t.include_items ? "" : "\n");
    if (t.include_items)
        out(c, ", %d item(s) listed\n", t.items);
    return 0;
}

/* ---- find ---------------------------------------------------------------------- */

typedef struct find_ctx {
    tool_ctx *c;
    sw_container cont;
    const char *query;
    const char *type_filter;
    int cs;
    int hits;
} find_ctx;

static int find_cb(void *ctx, const cJSON *item, const char *folder)
{
    find_ctx *f = ctx;
    const char *name = tdi_s(item, "Name");
    if (!name)
        return 0;
    int match;
    if (glob_has_wildcards(f->query)) {
        match = glob_match(f->query, name, f->cs);
    } else {
        char pat[512];
        snprintf(pat, sizeof pat, "*%s*", f->query);
        match = glob_match(pat, name, f->cs);
    }
    if (!match || (f->type_filter && *f->type_filter && _stricmp(short_type(item), f->type_filter) != 0))
        return 0;
    if (++f->hits > 200)
        return 1;
    print_item(f->c, f->cont, item, folder);
    return 0;
}

static int a_find(tool_ctx *c)
{
    nav_plc plc;
    sw_container cont;
    if (sw_plc(c, &plc) != 0 || get_container(c, &cont) != 0)
        return -1;
    const char *query = arg_req(c, "query");
    if (!query)
        return -1;
    th start = sw_folder(c, plc.software, cont, arg_s(c, "path"), 0, 0);
    if (!start)
        return -1;
    find_ctx f = { c, cont, query, arg_s(c, "typeFilter"), arg_b(c, "caseSensitive", 0), 0 };
    const char *path = arg_s(c, "path");
    sw_walk_group(start, cont, item_attrs(cont), path ? path : "", 0, find_cb, NULL, &f);
    if (f.hits > 200)
        out(c, "NOTICE: more than 200 matches, only the first 200 are shown - refine the query or the path.\n");
    else
        out(c, "%d match(es) for '%s' in %s.\n", f.hits, query, sw_container_name(cont));
    return 0;
}

/* ---- create / delete / rename ---------------------------------------------------- */

static int a_create_folder(tool_ctx *c)
{
    nav_plc plc;
    sw_container cont;
    if (sw_plc(c, &plc) != 0 || get_container(c, &cont) != 0)
        return -1;
    const char *path = arg_req(c, "path");
    if (!path)
        return -1;
    tool_ctx probe;
    ctx_init(&probe, c->tool, NULL);
    th existing = sw_folder(&probe, plc.software, cont, path, 0, 0);
    ctx_free(&probe);
    if (existing) {
        out(c, "Folder '%s' already exists in %s.\n", path, sw_container_name(cont));
        return 0;
    }
    if (!sw_folder(c, plc.software, cont, path, 1, arg_b(c, "createParents", 0)))
        return -1;
    out(c, "Folder '%s' ready in %s.\n", path, sw_container_name(cont));
    return 0;
}

static int a_delete_folder(tool_ctx *c)
{
    nav_plc plc;
    sw_container cont;
    if (sw_plc(c, &plc) != 0 || get_container(c, &cont) != 0)
        return -1;
    const char *path = arg_req(c, "path");
    if (!path)
        return -1;
    th group = sw_folder(c, plc.software, cont, path, 0, 0);
    if (!group)
        return -1;
    if (group == sw_container_root(plc.software, cont))
        return fail(c, "the container root cannot be deleted");
    cJSON *lists[2];
    int nitems = count_items(group, cont, lists, "Name");
    cJSON_Delete(lists[0]);
    cJSON_Delete(lists[1]);
    cJSON *groups = td_enum(td_get_h(group, "Groups"), NULL, -1);
    int nfolders = cJSON_GetArraySize(groups);
    cJSON_Delete(groups);
    if (nitems + nfolders > 0) {
        if (!arg_b(c, "deleteContents", 0) || !confirmed(c, "I understand"))
            return fail(c, "folder '%s' is not empty (%d item(s), %d subfolder(s)). Deleting it removes ALL contained items "
                           "and subfolders permanently: repeat with deleteContents=true and confirm='I understand'.",
                        path, nitems, nfolders);
    }
    if (td_call_v(group, "Delete", NULL) != 0)
        return fail_td(c, "deleting the folder failed");
    out(c, "Folder '%s' deleted%s.\n", path, nitems + nfolders ? " with its contents" : "");
    return 0;
}

static int a_rename_folder(tool_ctx *c)
{
    nav_plc plc;
    sw_container cont;
    if (sw_plc(c, &plc) != 0 || get_container(c, &cont) != 0)
        return -1;
    const char *path = arg_req(c, "path");
    const char *name = path ? arg_req(c, "newName") : NULL;
    if (!name)
        return -1;
    th group = sw_folder(c, plc.software, cont, path, 0, 0);
    if (!group)
        return -1;
    if (td_set(group, "Name", cJSON_CreateString(name)) != 0)
        return fail(c, "RenameFailed: %s. Openness does not allow renaming this kind of folder - rename it in the TIA Portal UI.",
                    td_err());
    out(c, "Folder '%s' renamed to '%s'.\n", path, name);
    return 0;
}

static const action_def actions[] = {
    { "create_folder", "deviceName, container, path; optional createParents=false",
      "Create the folder at path ('Motors/Drives'). Missing intermediate folders are an ERROR unless createParents=true.",
      a_create_folder, AF_PROJECT | AF_WRITES },
    { "delete_folder", "deviceName, container, path; optional deleteContents=false, confirm",
      "DESTRUCTIVE. An empty folder is deleted immediately; a non-empty one needs deleteContents=true AND confirm containing "
      "'I understand' (all contained items and subfolders are removed).",
      a_delete_folder, AF_PROJECT | AF_WRITES | AF_DESTRUCTIVE },
    { "find", "deviceName, container, query; optional path, typeFilter, caseSensitive=false",
      "Search items by name across the whole container: glob (* any sequence, ? one character) or substring. Returns "
      "'Name  [key=value, ...]' with the folder; capped at 200.",
      a_find, AF_PROJECT },
    { "get_tree", "deviceName, container; optional path, nested=true, includeItems=false, depth=0, typeFilter, limit=1000",
      "Folder tree with item/subfolder counts; includeItems=true also lists the items. depth=0 unlimited, 1 = immediate "
      "children. get_tree answers SHAPE, find answers IDENTITY.",
      a_get_tree, AF_PROJECT },
    { "rename_folder", "deviceName, container, path, newName",
      "Rename a folder. Openness refuses it for some containers (the error says so) - rename those in the TIA Portal UI.",
      a_rename_folder, AF_PROJECT | AF_WRITES },
};

const tool_def tool_folders = {
    .name = "folders",
    .title = "Project-tree folders",
    .summary = "Navigate and organise the folders (groups) of a PLC's software containers: program_blocks, tag_tables, "
               "udts, watch_tables, external_sources, technology_objects. Paths use '/' ('Motors/Drives').",
    .properties =
        "{"
        "\"deviceName\":{\"type\":\"string\",\"description\":\"Device or PLC name (session list_devices).\"},"
        "\"container\":{\"type\":\"string\",\"enum\":[\"program_blocks\",\"tag_tables\",\"udts\",\"watch_tables\",\"external_sources\",\"technology_objects\"],\"description\":\"Software container (default program_blocks).\"},"
        "\"path\":{\"type\":\"string\",\"description\":\"Folder path inside the container, '/' separated.\"},"
        "\"query\":{\"type\":\"string\",\"description\":\"find: glob pattern (* and ?) or substring.\"},"
        "\"newName\":{\"type\":\"string\",\"description\":\"rename_folder: new folder name.\"},"
        "\"createParents\":{\"type\":\"boolean\",\"description\":\"create_folder: create missing parent folders.\"},"
        "\"deleteContents\":{\"type\":\"boolean\",\"description\":\"delete_folder: allow deleting a non-empty folder.\"},"
        "\"confirm\":{\"type\":\"string\",\"description\":\"delete_folder: must contain 'I understand' for non-empty folders.\"},"
        "\"nested\":{\"type\":\"boolean\",\"description\":\"get_tree: indented tree (true) or flat path list (false).\"},"
        "\"includeItems\":{\"type\":\"boolean\",\"description\":\"get_tree: also list the items.\"},"
        "\"depth\":{\"type\":\"integer\",\"description\":\"get_tree: 0 unlimited, 1 immediate children.\"},"
        "\"typeFilter\":{\"type\":\"string\",\"description\":\"Item type filter, e.g. FB, FC, OB, GlobalDB, InstanceDB, PlcTagTable, PlcWatchTable.\"},"
        "\"caseSensitive\":{\"type\":\"boolean\",\"description\":\"find: case-sensitive matching.\"},"
        "\"limit\":{\"type\":\"integer\",\"description\":\"get_tree: maximum output lines (default 1000).\"}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
    .hints = 0,
};
