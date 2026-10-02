/* db: global and instance data blocks. */
#include "tools.h"

#include "tia/blockgen.h"
#include "tia/members.h"
#include "tia/session.h"
#include "tia/simaticml.h"
#include "tia/tia_dyn.h"
#include "tia/tia_sw.h"
#include "util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_db(const char *type)
{
    return strcmp(type, "GlobalDB") == 0 || strcmp(type, "InstanceDB") == 0 || strcmp(type, "ArrayDB") == 0 ||
           strstr(type, "DB") != NULL;
}

static int resolve_db(tool_ctx *c, nav_plc *plc, sw_found *db, int global_only)
{
    if (sw_plc(c, plc) != 0)
        return -1;
    const char *name = arg_req(c, "dbName");
    if (!name)
        return -1;
    snprintf(c->error_context, sizeof c->error_context, "device=%s db=%s", plc->device_name, name);
    if (sw_find(c, plc->software, SWC_BLOCKS, name, db) != 0)
        return -1;
    if (!is_db(db->type))
        return fail(c, "'%s' is a %s, not a data block", db->name, db->type);
    if (global_only && strcmp(db->type, "GlobalDB") != 0)
        return fail(c, "'%s' is an %s: its structure comes from %s. Change the owner instead (blocks_write / udt).", db->name,
                    db->type, strcmp(db->type, "InstanceDB") == 0 ? "the FB interface" : "its type");
    return 0;
}

/* ---- read ------------------------------------------------------------------------ */

typedef struct list_ctx {
    tool_ctx *c;
    int n;
} list_ctx;

static int list_cb(void *ctx, const cJSON *item, const char *folder)
{
    list_ctx *l = ctx;
    const char *type = short_type(item);
    if (!is_db(type))
        return 0;
    out(l->c, "%s%s%s  [type=%s, num=%lld", folder, *folder ? "/" : "", tdi_s(item, "Name"), type, tdi_i(item, "Number", 0));
    if (strcmp(type, "InstanceDB") == 0) {
        char *of = td_get_s(tdv_h(item), "InstanceOfName");
        if (of)
            out(l->c, ", instanceOf=%s", of);
        free(of);
    }
    out(l->c, "]\n");
    l->n++;
    return 0;
}

static int a_list(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    list_ctx l = { c, 0 };
    sw_walk(plc.software, SWC_BLOCKS, "Name,Number", list_cb, NULL, &l);
    out(c, "Total: %d data block(s)\n", l.n);
    return 0;
}

static void member_line(tool_ctx *c, mxml_node_t *m, const char *path, int indent, int show_path)
{
    const char *type = sml_attr(m, "Datatype");
    out(c, "%*s%s : %s", indent, "", show_path ? path : sml_attr(m, "Name"), type ? type : "?");
    const char *sv = sml_start_value(m);
    if (sv && *sv)
        out(c, " := %s", sv);
    const char *rem = sml_attr(m, "Remanence");
    if (rem && strcmp(rem, "NonRetain") != 0)
        out(c, "  [%s]", rem);
    const char *cm = sml_member_comment(m, NULL);
    if (cm && *cm)
        out(c, "  // %s", cm);
    out(c, "\n");
    int n = 0;
    for (mxml_node_t *se = sml_child(m, "Subelement"); se; se = sml_next(se, "Subelement")) {
        const char *idx = sml_attr(se, "Path");
        const char *v = sml_start_value(se);
        if (v && n < 64)
            out(c, "%*s[%s] := %s\n", indent + 4, "", idx ? idx : "?", v);
        n++;
    }
    if (n > 64)
        out(c, "%*s... %d element start values\n", indent + 4, "", n);
}

typedef struct flat_ctx {
    tool_ctx *c;
    int n;
} flat_ctx;

static int flat_cb(void *ctx, mxml_node_t *m, const char *section, const char *path, int depth)
{
    (void)section;
    (void)depth;
    flat_ctx *f = ctx;
    member_line(f->c, m, path, 0, 1);
    f->n++;
    return 0;
}

static int a_get_structure(tool_ctx *c)
{
    nav_plc plc;
    sw_found db;
    if (resolve_db(c, &plc, &db, 0) != 0)
        return -1;
    mxml_node_t *top = sw_export_tree(c, db.item);
    if (!top)
        return -1;
    out(c, "%s [%s]:\n", db.name, db.type);
    flat_ctx f = { c, 0 };
    sml_walk_members(sml_sections(sml_object(top)), -1, flat_cb, &f);
    out(c, "%d member(s)\n", f.n);
    mxmlDelete(top);
    return 0;
}

/* Splits "a.b[3]" into member path "a.b" and index "3". */
static void split_index(const char *path, char *member, size_t mcap, char *index, size_t icap)
{
    snprintf(member, mcap, "%s", path);
    index[0] = 0;
    char *br = strrchr(member, '[');
    if (br && member[strlen(member) - 1] == ']') {
        snprintf(index, icap, "%.*s", (int)(strlen(br) - 2), br + 1);
        *br = 0;
    }
}

typedef struct card_ctx {
    tool_ctx *c;
    int base_depth, max_depth;
} card_ctx;

static void print_children(tool_ctx *c, mxml_node_t *m, int depth, int max_depth)
{
    if (depth > max_depth)
        return;
    for (mxml_node_t *ch = sml_child(m, "Member"); ch; ch = sml_next(ch, "Member")) {
        member_line(c, ch, NULL, depth * 2, 0);
        print_children(c, ch, depth + 1, max_depth);
    }
    mxml_node_t *secs = sml_child(m, "Sections");
    for (mxml_node_t *s = sml_child(secs, "Section"); s; s = sml_next(s, "Section")) {
        out(c, "%*s<%s>\n", depth * 2, "", sml_attr(s, "Name") ? sml_attr(s, "Name") : "?");
        for (mxml_node_t *ch = sml_child(s, "Member"); ch; ch = sml_next(ch, "Member")) {
            member_line(c, ch, NULL, depth * 2 + 2, 0);
            print_children(c, ch, depth + 2, max_depth + 1);
        }
    }
}

static int a_get_member(tool_ctx *c)
{
    nav_plc plc;
    sw_found db;
    if (resolve_db(c, &plc, &db, 0) != 0)
        return -1;
    const char *path = arg_s(c, "path");
    if (!path || !*path)
        path = arg_s(c, "memberName");
    if (!path || !*path)
        return fail(c, "missing required argument 'path'");
    char member[512], index[64];
    split_index(path, member, sizeof member, index, sizeof index);
    ed_doc d;
    if (ed_open(c, session_project(), db.item, "Blocks", &d) != 0)
        return -1;
    const char *section = NULL;
    mxml_node_t *m = ed_member(c, &d, NULL, member, &section);
    if (m) {
        out(c, "%s.%s  [section=%s]\n", db.name, path, section ? section : "?");
        if (index[0]) {
            for (mxml_node_t *se = sml_child(m, "Subelement"); se; se = sml_next(se, "Subelement")) {
                if (sml_attr(se, "Path") && strcmp(sml_attr(se, "Path"), index) == 0) {
                    const char *v = sml_start_value(se);
                    out(c, "element [%s] start value: %s\n", index, v ? v : "(default)");
                }
            }
        }
        member_line(c, m, path, 0, 1);
        print_children(c, m, 1, (int)arg_i(c, "depth", 2));
    }
    ed_close(&d);
    return m ? 0 : -1;
}

/* ---- create / delete ----------------------------------------------------------------- */

static th target_group(tool_ctx *c, nav_plc *plc)
{
    return sw_folder(c, plc->software, SWC_BLOCKS, arg_s(c, "targetFolder"), 1, arg_b(c, "createParents", 0));
}

static int a_create(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *name = arg_req(c, "dbName");
    if (!name)
        return -1;
    tool_ctx probe;
    ctx_init(&probe, c->tool, NULL);
    sw_found ex;
    int exists = sw_find(&probe, plc.software, SWC_BLOCKS, name, &ex) == 0;
    ctx_free(&probe);
    if (exists)
        return fail(c, "a block named '%s' already exists (%s)", name, ex.type);
    th group = target_group(c, &plc);
    if (!group)
        return -1;
    bg_member *members = NULL;
    int nmembers = 0;
    if (bg_parse_interface(c, arg_get(c, "members"), plc.software, "Static", &members, &nmembers) != 0)
        return -1;
    bg_spec s;
    memset(&s, 0, sizeof s);
    s.kind = "GlobalDB";
    s.name = name;
    s.number = arg_i(c, "dbNumber", 0);
    s.language = "DB";
    s.author = arg_s(c, "author");
    s.version = arg_s(c, "version");
    s.memory_layout = arg_s(c, "memoryLayout");
    s.culture = mb_editing_language(session_project());
    s.members = members;
    s.nmembers = nmembers;
    mxml_node_t *top = bg_build_xml(&s);
    th db = mb_import_tree(c, group, "Blocks", top, 0);
    mxmlDelete(top);
    free(members);
    if (!db)
        return -1;
    long long num = 0;
    td_get_i(db, "Number", &num);
    out(c, "Global DB '%s' created (DB%lld)%s%s.\n", name, num, arg_s(c, "targetFolder") ? " in " : "",
        arg_s(c, "targetFolder") ? arg_s(c, "targetFolder") : "");
    return 0;
}

static int a_create_instance_db(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *name = arg_req(c, "dbName");
    const char *of = name ? arg_req(c, "instanceOfName") : NULL;
    if (!of)
        return -1;
    sw_found fb;
    if (sw_find(c, plc.software, SWC_BLOCKS, of, &fb) != 0)
        return -1;
    if (strcmp(fb.type, "FB") != 0)
        return fail(c, "'%s' is a %s: instance DBs need an FB", fb.name, fb.type);
    th group = target_group(c, &plc);
    if (!group)
        return -1;
    long long number = arg_i(c, "dbNumber", 0);
    th blocks = td_get_h(group, "Blocks");
    th db = td_call_h(blocks, "CreateInstanceDB", tda("sbis", name, number <= 0, number > 0 ? number : 0LL, fb.name));
    if (!db)
        return fail_td(c, "creating the instance DB failed");
    th comp = td_service(db, "Siemens.Engineering.Compiler.ICompilable");
    if (comp)
        session_compile(comp); /* the automatic number is assigned by the compiler */
    td_clear_err();
    long long num = 0;
    td_get_i(db, "Number", &num);
    if (num > 0)
        out(c, "Instance DB '%s' (DB%lld) of %s created.\n", name, num, fb.name);
    else
        out(c, "Instance DB '%s' of %s created (number assigned at the next compilation).\n", name, fb.name);
    return 0;
}

static int a_delete(tool_ctx *c)
{
    nav_plc plc;
    sw_found db;
    if (resolve_db(c, &plc, &db, 0) != 0)
        return -1;
    if (strcmp(db.type, "InstanceDB") == 0 && !arg_b(c, "force", 0))
        return fail(c, "'%s' is an instance DB: delete the owning FB's call or use blocks_write action=delete_block force=true",
                    db.name);
    if (td_call_v(db.item, "Delete", NULL) != 0)
        return fail_td(c, "delete failed");
    out(c, "Data block '%s' deleted.\n", db.name);
    return 0;
}

/* ---- members ---------------------------------------------------------------------------- */

static int finish(tool_ctx *c, ed_doc *d, const char *what, const char *dbname)
{
    th res = ed_commit(c, d);
    if (!res)
        return -1;
    out(c, "%s in %s.\n", what, dbname);
    return 0;
}

static int a_add_member(tool_ctx *c)
{
    nav_plc plc;
    sw_found db;
    if (resolve_db(c, &plc, &db, 1) != 0)
        return -1;
    const char *name = arg_req(c, "memberName");
    const char *type = name ? arg_req(c, "dataType") : NULL;
    if (!type)
        return -1;
    char dt[512];
    mb_normalize_datatype(plc.software, type, dt, sizeof dt);
    ed_doc d;
    if (ed_open(c, session_project(), db.item, "Blocks", &d) != 0)
        return -1;
    const char *start = arg_s(c, "initialValue");
    if (!start)
        start = arg_s(c, "startValue");
    if (ed_add_member(c, &d, "Static", name, dt, start, arg_s(c, "comment")) != 0) {
        ed_close(&d);
        return -1;
    }
    char what[700];
    snprintf(what, sizeof what, "Member '%s : %s' added", name, dt);
    return finish(c, &d, what, db.name);
}

static int count_top_members(ed_doc *d)
{
    int n = 0;
    for (mxml_node_t *s = sml_child(d->sections, "Section"); s; s = sml_next(s, "Section"))
        for (mxml_node_t *m = sml_child(s, "Member"); m; m = sml_next(m, "Member"))
            n++;
    return n;
}

static int a_delete_member(tool_ctx *c)
{
    nav_plc plc;
    sw_found db;
    if (resolve_db(c, &plc, &db, 1) != 0)
        return -1;
    const char *name = arg_req(c, "memberName");
    if (!name)
        return -1;
    ed_doc d;
    if (ed_open(c, session_project(), db.item, "Blocks", &d) != 0)
        return -1;
    mxml_node_t *m = ed_member(c, &d, NULL, name, NULL);
    if (!m) {
        ed_close(&d);
        return -1;
    }
    if (!strchr(name, '.') && count_top_members(&d) <= 1) {
        ed_close(&d);
        return fail(c, "'%s' is the last member of %s: delete the DB instead", name, db.name);
    }
    mxmlDelete(m);
    char what[600];
    snprintf(what, sizeof what, "Member '%s' deleted", name);
    return finish(c, &d, what, db.name);
}

static int a_update_member(tool_ctx *c)
{
    nav_plc plc;
    sw_found db;
    if (resolve_db(c, &plc, &db, 1) != 0)
        return -1;
    const char *name = arg_req(c, "memberName");
    if (!name)
        return -1;
    const char *nt = arg_s(c, "newDataType");
    const char *ns = arg_s(c, "newStartValue");
    const char *nc = arg_s(c, "newComment");
    const char *nn = arg_s(c, "newName");
    if (!nt && !ns && !nc && !nn)
        return fail(c, "pass at least one of newDataType, newStartValue, newComment, newName");
    char dt[512] = "";
    if (nt)
        mb_normalize_datatype(plc.software, nt, dt, sizeof dt);
    ed_doc d;
    if (ed_open(c, session_project(), db.item, "Blocks", &d) != 0)
        return -1;
    mxml_node_t *m = ed_member(c, &d, NULL, name, NULL);
    if (!m) {
        ed_close(&d);
        return -1;
    }
    ed_update_member(c, &d, m, nn, nt ? dt : NULL, ns, nc);
    th res = ed_commit(c, &d);
    if (!res) {
        if (nt && !ns)
            out(c, "Hint: when the type changes, pass a compatible newStartValue.\n");
        return -1;
    }
    out(c, "Member '%s' of %s updated.\n", name, db.name);
    return 0;
}

static int a_update_member_comment(tool_ctx *c)
{
    if (!arg_has(c, "newComment"))
        return fail(c, "missing required argument 'newComment' (not 'comment')");
    return a_update_member(c);
}

static int set_one(tool_ctx *c, ed_doc *d, const char *path, const char *value)
{
    char member[512], index[64];
    split_index(path, member, sizeof member, index, sizeof index);
    mxml_node_t *m = ed_member(c, d, NULL, member, NULL);
    if (!m)
        return -1;
    const char *dt = sml_attr(m, "Datatype");
    if (dt && (_stricmp(dt, "Struct") == 0 || (dt[0] == '"' && !index[0])))
        return fail(c, "'%s' is a %s: set the start values of its elements instead", member, dt);
    if (index[0])
        mb_set_element_start_value(m, index, value);
    else
        mb_set_start_value(m, value);
    return 0;
}

static int a_set_start_value(tool_ctx *c)
{
    nav_plc plc;
    sw_found db;
    if (resolve_db(c, &plc, &db, 0) != 0)
        return -1;
    const cJSON *values = arg_obj(c, "values");
    const char *path = arg_s(c, "path");
    if (!values && (!path || !arg_has(c, "value")))
        return fail(c, "pass path + value, or values {path: value, ...}");
    ed_doc d;
    if (ed_open(c, session_project(), db.item, "Blocks", &d) != 0)
        return -1;
    int n = 0;
    if (values) {
        const cJSON *v;
        cJSON_ArrayForEach(v, values)
        {
            char *text = tdv_text(v);
            int rc = set_one(c, &d, v->string, text);
            free(text);
            if (rc != 0) {
                ed_close(&d);
                return fail(c, "nothing was changed (all-or-nothing)");
            }
            n++;
        }
    } else {
        if (set_one(c, &d, path, arg_s(c, "value")) != 0) {
            ed_close(&d);
            return -1;
        }
        n = 1;
    }
    th res = ed_commit(c, &d);
    if (!res)
        return -1;
    out(c, "%d start value(s) set in %s. Download with reinitialisation is needed for the PLC to use new start values.\n", n,
        db.name);
    return 0;
}

static const action_def actions[] = {
    { "add_member", "deviceName, dbName, memberName, dataType; optional comment, initialValue",
      "Add a member to a global DB (dotted memberName adds inside a Struct). Supports all types incl. 'Array[0..9] of Int' "
      "and UDTs (names are quoted automatically). Global DBs only.",
      a_add_member, AF_PROJECT | AF_WRITES },
    { "create", "deviceName, dbName; optional dbNumber, members[], author, version, memoryLayout, targetFolder, createParents",
      "New global DB (number automatic if omitted), optionally with members [{name, dataType, startValue, comment}]. "
      "targetFolder ('Motors/Drives') must exist unless createParents=true. For instance DBs use create_instance_db.",
      a_create, AF_PROJECT | AF_WRITES },
    { "create_instance_db", "deviceName, dbName, instanceOfName; optional dbNumber, targetFolder, createParents",
      "Create an instance DB for an existing FB; TIA derives its structure from the FB.", a_create_instance_db,
      AF_PROJECT | AF_WRITES },
    { "delete", "deviceName, dbName; optional force=false", "Delete a DB. Instance DBs are refused unless force=true.",
      a_delete, AF_PROJECT | AF_WRITES | AF_DESTRUCTIVE },
    { "delete_member", "deviceName, dbName, memberName", "Remove a member from a global DB. Refuses the last member.",
      a_delete_member, AF_PROJECT | AF_WRITES },
    { "get_member", "deviceName, dbName, path; optional depth=2",
      "Member card at any depth ('a.b.c', 'arr[3]'): type, start value(s), comment, children. Works on global and "
      "instance DBs (incl. multi-instance chains).",
      a_get_member, AF_PROJECT },
    { "get_structure", "deviceName, dbName", "Full flat member list with types, start values, retain flags and comments.",
      a_get_structure, AF_PROJECT },
    { "list", "deviceName", "All global and instance DBs with folder-qualified names.", a_list, AF_PROJECT },
    { "set_start_value", "deviceName, dbName, path, value | values{path:value}",
      "Write start values at any depth on global AND instance DBs (structure is never changed). Array elements via "
      "[index] on the last segment. Values are TIA literals ('33', 'TRUE', 'T#2S', '16#00FF'). All-or-nothing.",
      a_set_start_value, AF_PROJECT | AF_WRITES },
    { "update_member", "deviceName, dbName, memberName; optional newDataType, newStartValue, newComment, newName",
      "Update a member of a global DB. Empty newStartValue/newComment removes them.", a_update_member,
      AF_PROJECT | AF_WRITES },
    { "update_member_comment", "deviceName, dbName, memberName, newComment",
      "Comment-only update. The argument is 'newComment', NOT 'comment'.", a_update_member_comment, AF_PROJECT | AF_WRITES },
};

const tool_def tool_db = {
    .name = "db",
    .title = "Data blocks",
    .summary = "Global and instance data blocks: list, structure, member cards, create, members add/update/delete, start "
               "values. Changes are applied by exporting the DB, editing the SimaticML and re-importing it.",
    .properties =
        "{"
        "\"deviceName\":{\"type\":\"string\",\"description\":\"Device or PLC name (session list_devices).\"},"
        "\"dbName\":{\"type\":\"string\",\"description\":\"Data block name (bare or folder-qualified).\"},"
        "\"memberName\":{\"type\":\"string\",\"description\":\"Member name; dotted path for nested members.\"},"
        "\"path\":{\"type\":\"string\",\"description\":\"Dotted member path, optional [index] on the last segment.\"},"
        "\"dataType\":{\"type\":\"string\",\"description\":\"TIA data type, e.g. Int, Real, String[20], Array[0..9] of Int, MyUdt.\"},"
        "\"initialValue\":{\"type\":\"string\",\"description\":\"add_member: start value (TIA literal).\"},"
        "\"comment\":{\"type\":\"string\",\"description\":\"add_member: member comment.\"},"
        "\"newDataType\":{\"type\":\"string\"},\"newStartValue\":{\"type\":\"string\"},"
        "\"newComment\":{\"type\":\"string\"},\"newName\":{\"type\":\"string\"},"
        "\"value\":{\"type\":\"string\",\"description\":\"set_start_value: TIA literal.\"},"
        "\"values\":{\"type\":\"object\",\"description\":\"set_start_value: {path: value, ...}.\"},"
        "\"members\":{\"type\":\"array\",\"items\":{\"type\":\"object\"},\"description\":\"create: [{name, dataType, startValue, comment}].\"},"
        "\"dbNumber\":{\"type\":\"integer\"},\"instanceOfName\":{\"type\":\"string\",\"description\":\"create_instance_db: FB name.\"},"
        "\"author\":{\"type\":\"string\"},\"version\":{\"type\":\"string\"},"
        "\"memoryLayout\":{\"type\":\"string\",\"enum\":[\"Optimized\",\"Standard\"]},"
        "\"targetFolder\":{\"type\":\"string\",\"description\":\"Folder path in program blocks ('Motors/Drives').\"},"
        "\"createParents\":{\"type\":\"boolean\"},\"force\":{\"type\":\"boolean\"},"
        "\"depth\":{\"type\":\"integer\",\"description\":\"get_member: children depth (default 2).\"}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
};
