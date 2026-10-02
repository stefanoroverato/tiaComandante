/* tag: PLC tag tables and tags. */
#include "tools.h"

#include "app/export_store.h"
#include "tia/members.h"
#include "tia/session.h"
#include "tia/tia_dyn.h"
#include "tia/tia_sw.h"
#include "util/fs.h"
#include "util/strbuf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG_ATTRS "Name,DataTypeName,LogicalAddress,ExternalAccessible,ExternalVisible,ExternalWritable"

static int resolve_table(tool_ctx *c, nav_plc *plc, sw_found *t)
{
    if (sw_plc(c, plc) != 0)
        return -1;
    const char *name = arg_req(c, "tagTableName");
    if (!name)
        return -1;
    snprintf(c->error_context, sizeof c->error_context, "device=%s table=%s", plc->device_name, name);
    return sw_find(c, plc->software, SWC_TAG_TABLES, name, t);
}

static th find_tag(tool_ctx *c, th table, const char *table_name, const char *tag)
{
    th tags = td_get_h(table, "Tags");
    th t = tags ? td_call_h(tags, "Find", tda("s", tag)) : 0;
    if (!t) {
        td_clear_err();
        fail(c, "tag '%s' not found in table '%s' (tag search: tag action=search)", tag, table_name);
    }
    return t;
}

/* First non-empty comment text of an object (any language). Caller frees. */
static char *any_comment(th obj)
{
    th mt = td_get_h(obj, "Comment");
    th items = mt ? td_get_h(mt, "Items") : 0;
    cJSON *list = items ? td_enum(items, "Text", -1) : NULL;
    char *res = NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        const char *t = tdi_s(it, "Text");
        if (t && *t) {
            res = _strdup(t);
            break;
        }
    }
    cJSON_Delete(list);
    td_clear_err();
    return res;
}

static void tag_line(tool_ctx *c, const cJSON *it, const char *prefix, int with_comment)
{
    const char *addr = tdi_s(it, "LogicalAddress");
    out(c, "%s%s  [type=%s%s%s", prefix ? prefix : "", tdi_s(it, "Name") ? tdi_s(it, "Name") : "?",
        tdi_s(it, "DataTypeName") ? tdi_s(it, "DataTypeName") : "?", addr && *addr ? ", address=" : "", addr && *addr ? addr : "");
    int acc = tdi_b(it, "ExternalAccessible", 1), vis = tdi_b(it, "ExternalVisible", 1), wr = tdi_b(it, "ExternalWritable", 1);
    if (!acc || !vis || !wr)
        out(c, ", accessible=%s, visible=%s, writable=%s", acc ? "true" : "false", vis ? "true" : "false", wr ? "true" : "false");
    if (with_comment) {
        char *cm = any_comment(tdv_h(it));
        if (cm)
            out(c, ", comment=%s", cm);
        free(cm);
    }
    out(c, "]\n");
}

/* ---- tables ------------------------------------------------------------------------- */

typedef struct list_ctx {
    tool_ctx *c;
    int n;
} list_ctx;

static int list_cb(void *ctx, const cJSON *item, const char *folder)
{
    list_ctx *l = ctx;
    print_item(l->c, SWC_TAG_TABLES, item, folder);
    l->n++;
    return 0;
}

static int a_list_tables(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    list_ctx l = { c, 0 };
    sw_walk(plc.software, SWC_TAG_TABLES, "Name", list_cb, NULL, &l);
    out(c, "Total: %d tag table(s)\n", l.n);
    return 0;
}

static int a_get_table_details(tool_ctx *c)
{
    nav_plc plc;
    sw_found t;
    if (resolve_table(c, &plc, &t) != 0)
        return -1;
    cJSON *tags = td_enum(td_get_h(t.item, "Tags"), TAG_ATTRS, -1);
    out(c, "Tag table %s%s%s (%d tags):\n", t.folder, *t.folder ? "/" : "", t.name, cJSON_GetArraySize(tags));
    const cJSON *it;
    cJSON_ArrayForEach(it, tags)
    tag_line(c, it, "", 1);
    cJSON_Delete(tags);
    cJSON *consts = td_enum(td_get_h(t.item, "UserConstants"), "Name,DataTypeName,Value", -1);
    if (cJSON_GetArraySize(consts) > 0) {
        out(c, "User constants:\n");
        cJSON_ArrayForEach(it, consts)
        out(c, "%s  [type=%s, value=%s]\n", tdi_s(it, "Name"), tdi_s(it, "DataTypeName") ? tdi_s(it, "DataTypeName") : "?",
            tdi_s(it, "Value") ? tdi_s(it, "Value") : "?");
    }
    cJSON_Delete(consts);
    return 0;
}

static int a_create_table(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *name = arg_req(c, "tagTableName");
    if (!name)
        return -1;
    tool_ctx probe;
    ctx_init(&probe, c->tool, NULL);
    sw_found ex;
    int exists = sw_find(&probe, plc.software, SWC_TAG_TABLES, name, &ex) == 0;
    ctx_free(&probe);
    if (exists)
        return fail(c, "tag table '%s' already exists", name);
    th group = sw_folder(c, plc.software, SWC_TAG_TABLES, arg_s(c, "targetFolder"), 1, arg_b(c, "createParents", 0));
    th tables = group ? td_get_h(group, "TagTables") : 0;
    if (!tables)
        return -1;
    if (!td_call_h(tables, "Create", tda("s", name)))
        return fail_td(c, "creating the tag table failed");
    out(c, "Tag table '%s' created.\n", name);
    return 0;
}

static int a_delete_table(tool_ctx *c)
{
    nav_plc plc;
    sw_found t;
    if (resolve_table(c, &plc, &t) != 0)
        return -1;
    int is_default = 0;
    td_get_b(t.item, "IsDefault", &is_default);
    if (is_default)
        return fail(c, "'%s' is the default tag table and cannot be deleted", t.name);
    long long n = 0;
    td_get_i(td_get_h(t.item, "Tags"), "Count", &n);
    if (td_call_v(t.item, "Delete", NULL) != 0)
        return fail_td(c, "delete failed");
    out(c, "Tag table '%s' and its %lld tag(s) deleted.\n", t.name, n);
    return 0;
}

static int a_move_table(tool_ctx *c)
{
    nav_plc plc;
    sw_found t;
    if (resolve_table(c, &plc, &t) != 0)
        return -1;
    if (!arg_has(c, "targetFolder"))
        return fail(c, "missing required argument 'targetFolder' ('' = container root)");
    th target = sw_folder(c, plc.software, SWC_TAG_TABLES, arg_s(c, "targetFolder"), 1, arg_b(c, "createParents", 0));
    if (!target || !mb_move(c, t.item, "TagTables", target))
        return -1;
    out(c, "Tag table '%s' moved to '%s'.\n", t.name, *arg_s(c, "targetFolder") ? arg_s(c, "targetFolder") : "(root)");
    return 0;
}

/* ---- tags ------------------------------------------------------------------------------ */

static int a_add_tag(tool_ctx *c)
{
    nav_plc plc;
    sw_found t;
    if (resolve_table(c, &plc, &t) != 0)
        return -1;
    const char *name = arg_req(c, "tagName");
    const char *type = name ? arg_req(c, "dataType") : NULL;
    if (!type)
        return -1;
    const char *addr = arg_s(c, "logicalAddress");
    th tags = td_get_h(t.item, "Tags");
    th tag = td_call_h(tags, "Create", tda("sss", name, type, addr && *addr ? addr : ""));
    if (!tag)
        return fail_td(c, "creating the tag failed");
    const char *comment = arg_s(c, "comment");
    if (comment && *comment && mb_ml_set(tag, "Comment", mb_editing_language(session_project()), comment) != 0)
        out(c, "Warning: the comment could not be set (%s).\n", td_err());
    char *real_addr = td_get_s(tag, "LogicalAddress");
    out(c, "Tag '%s' (%s%s%s) added to '%s'.\n", name, type, real_addr && *real_addr ? " at " : "", real_addr ? real_addr : "", t.name);
    free(real_addr);
    return 0;
}

static int tag_has_custom(th tag, char **comment)
{
    *comment = any_comment(tag);
    int acc = 1, vis = 1, wr = 1;
    td_get_b(tag, "ExternalAccessible", &acc);
    td_get_b(tag, "ExternalVisible", &vis);
    td_get_b(tag, "ExternalWritable", &wr);
    td_clear_err();
    return *comment != NULL || !acc || !vis || !wr;
}

static int a_delete_tag(tool_ctx *c)
{
    nav_plc plc;
    sw_found t;
    if (resolve_table(c, &plc, &t) != 0)
        return -1;
    const cJSON *names = arg_arr(c, "tagNames");
    cJSON *list = cJSON_CreateArray();
    if (names) {
        const cJSON *n;
        cJSON_ArrayForEach(n, names)
        if (cJSON_IsString(n))
            cJSON_AddItemToArray(list, cJSON_CreateString(n->valuestring));
    } else {
        const char *n = arg_req(c, "tagName");
        if (!n) {
            cJSON_Delete(list);
            return -1;
        }
        cJSON_AddItemToArray(list, cJSON_CreateString(n));
    }
    int deleted = 0;
    const cJSON *n;
    cJSON_ArrayForEach(n, list)
    {
        th tag = find_tag(c, t.item, t.name, n->valuestring);
        if (!tag)
            break;
        char *cm = NULL;
        if (tag_has_custom(tag, &cm) && !confirmed(c, "I understand")) {
            fail(c, "tag '%s' has a comment or non-default access flags that add_tag cannot restore%s%s. Repeat with "
                    "confirm='I understand this destroys the comment' (or use tag action=move).",
                 n->valuestring, cm ? ": " : "", cm ? cm : "");
            free(cm);
            break;
        }
        free(cm);
        if (td_call_v(tag, "Delete", NULL) != 0) {
            fail_td(c, "delete failed");
            break;
        }
        deleted++;
    }
    out(c, "%d tag(s) deleted from '%s'.\n", deleted, t.name);
    cJSON_Delete(list);
    return c->is_error ? -1 : 0;
}

static int a_update_comment(tool_ctx *c)
{
    nav_plc plc;
    sw_found t;
    if (resolve_table(c, &plc, &t) != 0)
        return -1;
    const char *name = arg_req(c, "tagName");
    if (!name)
        return -1;
    if (!arg_has(c, "newComment"))
        return fail(c, "missing required argument 'newComment' (not 'comment')");
    th tag = find_tag(c, t.item, t.name, name);
    if (!tag)
        return -1;
    if (mb_ml_set(tag, "Comment", mb_editing_language(session_project()), arg_s(c, "newComment")) != 0)
        return fail_td(c, "setting the comment failed");
    out(c, "Comment of '%s' updated.\n", name);
    return 0;
}

static int a_set_access(tool_ctx *c)
{
    nav_plc plc;
    sw_found t;
    if (resolve_table(c, &plc, &t) != 0)
        return -1;
    const char *name = arg_req(c, "tagName");
    if (!name)
        return -1;
    if (!arg_has(c, "accessible") && !arg_has(c, "visible") && !arg_has(c, "writable"))
        return fail(c, "pass accessible, visible and/or writable (NOT externalAccessible...)");
    th tag = find_tag(c, t.item, t.name, name);
    if (!tag)
        return -1;
    static const struct {
        const char *arg, *prop;
    } map[] = { { "accessible", "ExternalAccessible" }, { "visible", "ExternalVisible" }, { "writable", "ExternalWritable" } };
    for (int i = 0; i < 3; i++) {
        if (!arg_has(c, map[i].arg))
            continue;
        if (td_set(tag, map[i].prop, cJSON_CreateBool(arg_b(c, map[i].arg, 1))) != 0)
            return fail_td(c, map[i].prop);
    }
    cJSON *a = td_attrs(tag, "ExternalAccessible,ExternalVisible,ExternalWritable");
    out(c, "Access of '%s': accessible=%s, visible=%s, writable=%s\n", name,
        tdv_b(cJSON_GetObjectItemCaseSensitive(a, "ExternalAccessible"), 0) ? "true" : "false",
        tdv_b(cJSON_GetObjectItemCaseSensitive(a, "ExternalVisible"), 0) ? "true" : "false",
        tdv_b(cJSON_GetObjectItemCaseSensitive(a, "ExternalWritable"), 0) ? "true" : "false");
    cJSON_Delete(a);
    return 0;
}

static int a_rename(tool_ctx *c)
{
    nav_plc plc;
    sw_found t;
    if (resolve_table(c, &plc, &t) != 0)
        return -1;
    const char *name = arg_req(c, "tagName");
    const char *nn = name ? arg_req(c, "newName") : NULL;
    if (!nn)
        return -1;
    th tag = find_tag(c, t.item, t.name, name);
    if (!tag)
        return -1;
    if (td_set(tag, "Name", cJSON_CreateString(nn)) != 0)
        return fail_td(c, "rename failed");
    out(c, "Tag '%s' renamed to '%s'. Program references follow the tag symbolically.\n", name, nn);
    return 0;
}

typedef struct search_ctx {
    tool_ctx *c;
    const char *q;
    int n;
} search_ctx;

static int search_cb(void *ctx, const cJSON *item, const char *folder)
{
    search_ctx *s = ctx;
    cJSON *tags = td_enum(td_get_h(tdv_h(item), "Tags"), TAG_ATTRS, -1);
    const cJSON *it;
    char prefix[600];
    snprintf(prefix, sizeof prefix, "%s%s%s/", folder, *folder ? "/" : "", tdi_s(item, "Name") ? tdi_s(item, "Name") : "?");
    cJSON_ArrayForEach(it, tags)
    {
        const char *name = tdi_s(it, "Name");
        const char *addr = tdi_s(it, "LogicalAddress");
        size_t ql = strlen(s->q);
        int hit = 0;
        for (const char *p = name ? name : ""; *p && !hit; p++)
            hit = _strnicmp(p, s->q, ql) == 0;
        if (!hit && addr)
            hit = _stricmp(addr, s->q) == 0;
        if (hit && s->n++ < 200)
            tag_line(s->c, it, prefix, 0);
    }
    cJSON_Delete(tags);
    return 0;
}

static int a_search(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *q = arg_req(c, "tagName");
    if (!q)
        return -1;
    search_ctx s = { c, q, 0 };
    sw_walk(plc.software, SWC_TAG_TABLES, "Name", search_cb, NULL, &s);
    out(c, "%d match(es)%s.\n", s.n, s.n > 200 ? " (first 200 shown)" : "");
    return 0;
}

static int a_move(tool_ctx *c)
{
    nav_plc plc;
    sw_found t;
    if (resolve_table(c, &plc, &t) != 0)
        return -1;
    const char *name = arg_req(c, "tagName");
    const char *target_name = name ? arg_req(c, "targetTable") : NULL;
    if (!target_name)
        return -1;
    sw_found target;
    if (sw_find(c, plc.software, SWC_TAG_TABLES, target_name, &target) != 0)
        return -1;
    th tag = find_tag(c, t.item, t.name, name);
    if (!tag)
        return -1;
    cJSON *a = td_attrs(tag, TAG_ATTRS);
    char type[256], addr[128];
    snprintf(type, sizeof type, "%s", tdv_s(cJSON_GetObjectItemCaseSensitive(a, "DataTypeName")) ? tdv_s(cJSON_GetObjectItemCaseSensitive(a, "DataTypeName")) : "Bool");
    snprintf(addr, sizeof addr, "%s", tdv_s(cJSON_GetObjectItemCaseSensitive(a, "LogicalAddress")) ? tdv_s(cJSON_GetObjectItemCaseSensitive(a, "LogicalAddress")) : "");
    int acc = tdv_b(cJSON_GetObjectItemCaseSensitive(a, "ExternalAccessible"), 1);
    int vis = tdv_b(cJSON_GetObjectItemCaseSensitive(a, "ExternalVisible"), 1);
    int wr = tdv_b(cJSON_GetObjectItemCaseSensitive(a, "ExternalWritable"), 1);
    cJSON_Delete(a);
    const char *culture = mb_editing_language(session_project());
    char *comment = mb_ml_get(tag, "Comment", culture);
    if (td_call_v(tag, "Delete", NULL) != 0) {
        free(comment);
        return fail_td(c, "removing the tag from the source table failed");
    }
    th tags_dst = td_get_h(target.item, "Tags");
    th moved = td_call_h(tags_dst, "Create", tda("sss", name, type, addr));
    th dest_table = target.item;
    if (!moved) {
        char err[1024];
        snprintf(err, sizeof err, "%s", td_err());
        th back = td_call_h(td_get_h(t.item, "Tags"), "Create", tda("sss", name, type, addr));
        moved = back;
        dest_table = 0;
        fail(c, "creating the tag in '%s' failed: %s%s", target.name, err, back ? " (restored in the source table)" : " (RESTORE FAILED)");
    }
    if (moved) {
        if (comment && *comment)
            mb_ml_set(moved, "Comment", culture, comment);
        td_set(moved, "ExternalAccessible", cJSON_CreateBool(acc));
        td_set(moved, "ExternalVisible", cJSON_CreateBool(vis));
        td_set(moved, "ExternalWritable", cJSON_CreateBool(wr));
        td_clear_err();
    }
    free(comment);
    if (!dest_table)
        return -1;
    out(c, "Tag '%s' moved from '%s' to '%s' (comment and access flags preserved).\n", name, t.name, target.name);
    return 0;
}

/* ---- export / import -------------------------------------------------------------------- */

static void csv_field(strbuf *sb, const char *s)
{
    int quote = s && (strchr(s, ';') || strchr(s, '"') || strchr(s, '\n'));
    if (quote)
        sb_appendc(sb, '"');
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '"')
            sb_appendc(sb, '"');
        sb_appendc(sb, *p);
    }
    if (quote)
        sb_appendc(sb, '"');
}

static int a_export_tag_table_data(tool_ctx *c)
{
    nav_plc plc;
    sw_found t;
    if (resolve_table(c, &plc, &t) != 0)
        return -1;
    const char *format = arg_s(c, "format");
    if (!format || !*format)
        format = "csv";
    const char *outp = arg_s(c, "outputPath");
    int inline_ = arg_b(c, "returnInline", outp && *outp ? 0 : 1);
    char path[1024], name[300];
    if (_stricmp(format, "xml") == 0) {
        if (sw_export_xml(c, t.item, path, sizeof path) != 0)
            return -1;
        snprintf(name, sizeof name, "%s.xml", t.name);
        int rc = sw_deliver(c, path, name, "application/xml", outp, inline_);
        fs_remove(path);
        return rc;
    }
    if (_stricmp(format, "csv") != 0)
        return fail(c, "format '%s' is not supported yet: use csv or xml (xlsx is planned)", format);
    strbuf sb;
    sb_init(&sb);
    sb_append(&sb, "\xEF\xBB\xBFName;DataType;Address;Comment;Accessible;Visible;Writable\r\n");
    cJSON *tags = td_enum(td_get_h(t.item, "Tags"), TAG_ATTRS, -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, tags)
    {
        char *cm = any_comment(tdv_h(it));
        csv_field(&sb, tdi_s(it, "Name"));
        sb_appendc(&sb, ';');
        csv_field(&sb, tdi_s(it, "DataTypeName"));
        sb_appendc(&sb, ';');
        csv_field(&sb, tdi_s(it, "LogicalAddress"));
        sb_appendc(&sb, ';');
        csv_field(&sb, cm);
        sb_printf(&sb, ";%s;%s;%s\r\n", tdi_b(it, "ExternalAccessible", 1) ? "true" : "false",
                  tdi_b(it, "ExternalVisible", 1) ? "true" : "false", tdi_b(it, "ExternalWritable", 1) ? "true" : "false");
        free(cm);
    }
    cJSON_Delete(tags);
    if (fs_temp_path("tags", ".csv", path, sizeof path) != 0 || fs_write_all(path, sb.p, sb.len) != 0) {
        sb_free(&sb);
        return fail(c, "cannot write the CSV");
    }
    sb_free(&sb);
    snprintf(name, sizeof name, "%s.csv", t.name);
    int rc = sw_deliver(c, path, name, "text/csv", outp, inline_);
    fs_remove(path);
    return rc;
}

static int a_import_table(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    char path[1024];
    int temp = 0;
    const char *file = arg_s(c, "filePath");
    const char *xml = arg_s(c, "xmlContent");
    const char *id = arg_s(c, "exportId");
    if (file && *file) {
        if (fs_full_path(file, path, sizeof path) != 0 || !fs_is_file(path))
            return fail(c, "file '%s' not found", file);
    } else if (xml && *xml) {
        if (fs_temp_path("inline", ".xml", path, sizeof path) != 0 || fs_write_all(path, xml, strlen(xml)) != 0)
            return fail(c, "cannot write a temporary file");
        temp = 1;
    } else if (id && *id) {
        export_info e;
        if (export_get(id, &e) != 0)
            return fail(c, "export '%s' not found", id);
        snprintf(path, sizeof path, "%s", e.path);
    } else {
        return fail(c, "pass filePath, xmlContent or exportId");
    }
    th group = sw_folder(c, plc.software, SWC_TAG_TABLES, arg_s(c, "targetFolder"), 1, arg_b(c, "createParents", 0));
    th res = group ? mb_import_file(c, group, "TagTables", path, arg_b(c, "overwrite", 0)) : 0;
    if (temp)
        fs_remove(path);
    if (!res)
        return -1;
    char *name = td_get_s(res, "Name");
    out(c, "Tag table '%s' imported.\n", name ? name : "?");
    free(name);
    return 0;
}

/* ---- address occupancy --------------------------------------------------------------------- */

typedef struct span {
    char area;     /* M, I, Q */
    long byte;
    int bit;       /* -1 for byte/word/... */
    int size;      /* bytes (0 for a bit) */
    char tag[200];
    char table[200];
} span;

typedef struct spans {
    span *v;
    int n, cap;
} spans;

/* Parses %M10.3, %MB5, %MW20, %MD4, %IW64, %Q0.0, "%I0.0:P". */
static int parse_address(const char *a, span *s)
{
    if (!a)
        return -1;
    while (*a == ' ')
        a++;
    if (*a == '%')
        a++;
    char area = (char)toupper((unsigned char)*a);
    if (area == 'E')
        area = 'I';
    if (area == 'A')
        area = 'Q';
    if (area != 'M' && area != 'I' && area != 'Q')
        return -1;
    a++;
    int size = 0;
    char unit = (char)toupper((unsigned char)*a);
    if (unit == 'B' || unit == 'W' || unit == 'D' || unit == 'L') {
        size = unit == 'B' ? 1 : unit == 'W' ? 2 : unit == 'D' ? 4 : 8;
        a++;
    } else if (unit == 'X') {
        a++;
    }
    if (!isdigit((unsigned char)*a))
        return -1;
    char *end;
    long byte = strtol(a, &end, 10);
    s->area = area;
    s->byte = byte;
    s->size = size;
    s->bit = -1;
    if (!size) {
        if (*end != '.')
            return -1;
        s->bit = (int)strtol(end + 1, NULL, 10);
    }
    return 0;
}

static int type_size(const char *t, int *is_bit)
{
    *is_bit = 0;
    if (!t || _stricmp(t, "Bool") == 0) {
        *is_bit = 1;
        return 0;
    }
    static const struct {
        const char *n;
        int s;
    } sizes[] = { { "Byte", 1 },  { "Char", 1 },  { "SInt", 1 },  { "USInt", 1 }, { "Word", 2 },  { "Int", 2 },
                  { "UInt", 2 },  { "WChar", 2 }, { "Date", 2 },  { "S5Time", 2 }, { "DWord", 4 }, { "DInt", 4 },
                  { "UDInt", 4 }, { "Real", 4 },  { "Time", 4 },  { "TOD", 4 },   { "Time_Of_Day", 4 },
                  { "LWord", 8 }, { "LInt", 8 },  { "ULInt", 8 }, { "LReal", 8 }, { "LTime", 8 } };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        if (_stricmp(t, sizes[i].n) == 0)
            return sizes[i].s;
    return -1;
}

typedef struct collect_ctx {
    spans *sp;
    int used_only;
} collect_ctx;

static int tag_is_used(th tag)
{
    th svc = td_service(tag, "Siemens.Engineering.CrossReference.CrossReferenceService");
    th res = svc ? td_call_h(svc, "GetCrossReferences", tda("e", "Siemens.Engineering.CrossReference.CrossReferenceFilter", "AllObjects")) : 0;
    cJSON *srcs = res ? td_enum(td_get_h(res, "Sources"), NULL, -1) : NULL;
    int used = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, srcs)
    {
        cJSON *refs = td_enum(td_get_h(tdv_h(s), "References"), NULL, -1);
        used |= cJSON_GetArraySize(refs) > 0;
        cJSON_Delete(refs);
    }
    cJSON_Delete(srcs);
    td_clear_err();
    return used;
}

static int collect_cb(void *ctx, const cJSON *item, const char *folder)
{
    (void)folder;
    collect_ctx *k = ctx;
    cJSON *tags = td_enum(td_get_h(tdv_h(item), "Tags"), "Name,DataTypeName,LogicalAddress", -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, tags)
    {
        span s;
        memset(&s, 0, sizeof s);
        if (parse_address(tdi_s(it, "LogicalAddress"), &s) != 0)
            continue;
        if (s.size == 0 && s.bit < 0)
            continue;
        if (s.bit >= 0) {
            int is_bit;
            int sz = type_size(tdi_s(it, "DataTypeName"), &is_bit);
            if (!is_bit && sz > 0) {
                s.size = sz;
                s.bit = -1;
            }
        }
        if (k->used_only && !tag_is_used(tdv_h(it)))
            continue;
        snprintf(s.tag, sizeof s.tag, "%s", tdi_s(it, "Name") ? tdi_s(it, "Name") : "?");
        snprintf(s.table, sizeof s.table, "%s", tdi_s(item, "Name") ? tdi_s(item, "Name") : "?");
        if (k->sp->n == k->sp->cap) {
            int cap = k->sp->cap ? k->sp->cap * 2 : 128;
            span *p = realloc(k->sp->v, (size_t)cap * sizeof *p);
            if (!p)
                break;
            k->sp->v = p;
            k->sp->cap = cap;
        }
        k->sp->v[k->sp->n++] = s;
    }
    cJSON_Delete(tags);
    return 0;
}

static int span_cmp(const void *a, const void *b)
{
    const span *x = a, *y = b;
    if (x->area != y->area)
        return x->area - y->area;
    if (x->byte != y->byte)
        return x->byte < y->byte ? -1 : 1;
    return x->bit - y->bit;
}

static int overlaps(const span *a, const span *b)
{
    if (a->area != b->area)
        return 0;
    long a0 = a->byte, a1 = a->byte + (a->size ? a->size - 1 : 0);
    long b0 = b->byte, b1 = b->byte + (b->size ? b->size - 1 : 0);
    if (a1 < b0 || b1 < a0)
        return 0;
    if (a->bit >= 0 && b->bit >= 0)
        return a->bit == b->bit && a->byte == b->byte;
    return 1;
}

static int load_spans(tool_ctx *c, nav_plc *plc, spans *sp, int used_only)
{
    memset(sp, 0, sizeof *sp);
    collect_ctx k = { sp, used_only };
    sw_walk(plc->software, SWC_TAG_TABLES, "Name", collect_cb, NULL, &k);
    if (sp->n > 1)
        qsort(sp->v, (size_t)sp->n, sizeof *sp->v, span_cmp);
    (void)c;
    return 0;
}

static int span_free_at(const spans *sp, char area, long byte, int bit, int size)
{
    span probe = { area, byte, bit, size, "", "" };
    for (int i = 0; i < sp->n; i++)
        if (overlaps(&sp->v[i], &probe))
            return 0;
    return 1;
}

static int a_find_next_free(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *area_s = arg_s(c, "area");
    char area = area_s && *area_s ? (char)toupper((unsigned char)area_s[0]) : 'M';
    if (area != 'M' && area != 'I' && area != 'Q')
        return fail(c, "area must be M, I or Q");
    long start = (long)arg_i(c, "startByte", 0);
    const char *dtype = arg_s(c, "dataType");
    int is_bit;
    int size = type_size(dtype && *dtype ? dtype : "Bool", &is_bit);
    if (size < 0)
        return fail(c, "unsupported dataType '%s' (use an elementary type: Bool, Byte, Word, Int, DWord, Real, ...)", dtype);
    const char *mode = arg_s(c, "mode");
    int used_only = mode && _stricmp(mode, "used") == 0;
    spans sp;
    load_spans(c, &plc, &sp, used_only);
    char addr[64] = "";
    for (long b = start; b < 65536 && !*addr; b++) {
        if (is_bit) {
            for (int bit = 0; bit < 8; bit++)
                if (span_free_at(&sp, area, b, bit, 0)) {
                    snprintf(addr, sizeof addr, "%%%c%ld.%d", area, b, bit);
                    break;
                }
        } else {
            if (size >= 2 && (b % 2) != 0)
                continue;
            if (span_free_at(&sp, area, b, -1, size)) {
                char unit = size == 1 ? 'B' : size == 2 ? 'W' : size == 4 ? 'D' : 'L';
                snprintf(addr, sizeof addr, "%%%c%c%ld", area, unit, b);
            }
        }
    }
    free(sp.v);
    if (!*addr)
        return fail(c, "no free address found");
    out(c, "Next free %s address in %c area from byte %ld (%s tags): %s\n", dtype && *dtype ? dtype : "Bool", area, start,
        used_only ? "used" : "declared", addr);
    return 0;
}

static int a_get_assignment_list(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *area_s = arg_s(c, "area");
    char area = area_s && *area_s && _stricmp(area_s, "all") != 0 ? (char)toupper((unsigned char)area_s[0]) : 0;
    const char *mode = arg_s(c, "mode");
    int used_only = mode && _stricmp(mode, "used") == 0;
    int conflicts = mode && _stricmp(mode, "conflicts") == 0;
    spans sp;
    load_spans(c, &plc, &sp, used_only);
    int shown = 0, nconf = 0;
    for (int i = 0; i < sp.n; i++) {
        const span *s = &sp.v[i];
        if (area && s->area != area)
            continue;
        if (conflicts) {
            for (int j = i + 1; j < sp.n; j++) {
                if (overlaps(s, &sp.v[j])) {
                    out(c, "CONFLICT %s (%s) <-> %s (%s)\n", s->tag, s->table, sp.v[j].tag, sp.v[j].table);
                    nconf++;
                }
            }
            continue;
        }
        if (s->bit >= 0)
            out(c, "%%%c%ld.%d  %s  [table=%s]\n", s->area, s->byte, s->bit, s->tag, s->table);
        else
            out(c, "%%%c%c%ld  %s  [bytes=%d, table=%s]\n", s->area, s->size == 1 ? 'B' : s->size == 2 ? 'W' : s->size == 4 ? 'D' : 'L',
                s->byte, s->tag, s->size, s->table);
        shown++;
    }
    if (conflicts)
        out(c, "%d overlapping address pair(s).\n", nconf);
    else
        out(c, "%d %s address(es)%s. Hardware I/O addresses of modules are listed by the hardware tool.\n", shown,
            used_only ? "used" : "declared", area ? "" : " in M/I/Q");
    free(sp.v);
    return 0;
}

static const action_def actions[] = {
    { "add_tag", "deviceName, tagTableName, tagName, dataType; optional logicalAddress, comment",
      "Add a tag. dataType: Bool, Int, Real, ...; logicalAddress: %M0.0, %MW100, %I1.2 (omit for an automatic address).",
      a_add_tag, AF_PROJECT | AF_WRITES },
    { "create_table", "deviceName, tagTableName; optional targetFolder, createParents=false",
      "New empty tag table, optionally inside a folder ('Motors/Drives'; missing folders need createParents=true).",
      a_create_table, AF_PROJECT | AF_WRITES },
    { "delete_table", "deviceName, tagTableName", "Delete a tag table and all its tags (not the default table).",
      a_delete_table, AF_PROJECT | AF_WRITES | AF_DESTRUCTIVE },
    { "delete_tag", "deviceName, tagTableName, tagName; optional tagNames[], confirm",
      "Delete one or several tags. REFUSES when a tag has a comment or non-default access flags unless confirm='I "
      "understand this destroys the comment' (use move to relocate tags).",
      a_delete_tag, AF_PROJECT | AF_WRITES | AF_DESTRUCTIVE },
    { "export_tag_table_data", "deviceName, tagTableName, format=csv|xml; optional outputPath, returnInline",
      "Export a tag table as CSV (Name;DataType;Address;Comment;Accessible;Visible;Writable) or raw TIA XML for "
      "import_table. xlsx is not available yet.",
      a_export_tag_table_data, AF_PROJECT },
    { "find_next_free", "deviceName; optional area=M|I|Q, startByte=0, dataType=Bool, mode=declared|used",
      "Next free address. mode=declared (default, safest) skips every declared tag address; mode=used skips only tags "
      "referenced in code. Words and larger are aligned to even bytes.",
      a_find_next_free, AF_PROJECT },
    { "get_assignment_list", "deviceName; optional area=M|I|Q|all, mode=declared|used|conflicts",
      "Address occupancy from the tag tables: declared, used in code, or overlapping (conflicts).", a_get_assignment_list,
      AF_PROJECT },
    { "get_table_details", "deviceName, tagTableName", "All tags with type, address, access flags and comment, plus user constants.",
      a_get_table_details, AF_PROJECT },
    { "import_table", "deviceName, filePath OR xmlContent OR exportId; optional overwrite=false, targetFolder, createParents",
      "OFFLINE REQUIRED. Import a tag table from TIA XML (as written by export_tag_table_data format=xml).",
      a_import_table, AF_PROJECT | AF_WRITES | AF_OFFLINE },
    { "list_tables", "deviceName", "All tag tables: 'Folder/Name  [type=PlcTagTable, tags=N]'.", a_list_tables, AF_PROJECT },
    { "move", "deviceName, tagTableName, tagName, targetTable",
      "Move ONE tag to another table, preserving comment and access flags.", a_move, AF_PROJECT | AF_WRITES },
    { "move_table", "deviceName, tagTableName, targetFolder; optional createParents=false",
      "OFFLINE REQUIRED. Move a tag table between folders ('' = root): export -> delete -> import, restored on failure.",
      a_move_table, AF_PROJECT | AF_WRITES | AF_OFFLINE },
    { "rename", "deviceName, tagTableName, tagName, newName",
      "Rename a tag (supported by Openness V21; program code refers to tags symbolically).", a_rename, AF_PROJECT | AF_WRITES },
    { "search", "deviceName, tagName", "Substring match on tag names (or exact address) across all tables.", a_search,
      AF_PROJECT },
    { "set_access", "deviceName, tagTableName, tagName; accessible, visible, writable",
      "Set HMI/OPC UA access flags. The arguments are accessible/visible/writable, NOT externalAccessible etc.",
      a_set_access, AF_PROJECT | AF_WRITES },
    { "update_comment", "deviceName, tagTableName, tagName, newComment",
      "Set the tag comment in the project editing language. The argument is 'newComment', NOT 'comment'.",
      a_update_comment, AF_PROJECT | AF_WRITES },
};

const tool_def tool_tag = {
    .name = "tag",
    .title = "PLC tags",
    .summary = "PLC tag tables and tags: list, details, create/delete, comments, access flags, move/rename, search, CSV/XML "
               "export and import, address occupancy and next free address.",
    .properties =
        "{"
        "\"deviceName\":{\"type\":\"string\",\"description\":\"Device or PLC name (session list_devices).\"},"
        "\"tagTableName\":{\"type\":\"string\",\"description\":\"Tag table name (bare or folder-qualified).\"},"
        "\"tagName\":{\"type\":\"string\"},\"tagNames\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"dataType\":{\"type\":\"string\"},\"logicalAddress\":{\"type\":\"string\",\"description\":\"e.g. %M0.0, %MW100\"},"
        "\"comment\":{\"type\":\"string\"},\"newComment\":{\"type\":\"string\"},\"newName\":{\"type\":\"string\"},"
        "\"accessible\":{\"type\":\"boolean\"},\"visible\":{\"type\":\"boolean\"},\"writable\":{\"type\":\"boolean\"},"
        "\"targetTable\":{\"type\":\"string\"},\"targetFolder\":{\"type\":\"string\"},\"createParents\":{\"type\":\"boolean\"},"
        "\"confirm\":{\"type\":\"string\"},"
        "\"format\":{\"type\":\"string\",\"enum\":[\"csv\",\"xml\",\"xlsx\"]},\"outputPath\":{\"type\":\"string\"},"
        "\"returnInline\":{\"type\":\"boolean\"},"
        "\"filePath\":{\"type\":\"string\"},\"xmlContent\":{\"type\":\"string\"},\"exportId\":{\"type\":\"string\"},"
        "\"overwrite\":{\"type\":\"boolean\"},"
        "\"area\":{\"type\":\"string\",\"enum\":[\"M\",\"I\",\"Q\",\"all\"]},\"startByte\":{\"type\":\"integer\"},"
        "\"mode\":{\"type\":\"string\",\"enum\":[\"declared\",\"used\",\"conflicts\"]}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
};
