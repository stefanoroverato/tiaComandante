/* watch: watch and force tables. Entries are edited through a SimaticML round
   trip because Openness V21 exposes entry properties read-only. */
#include "tools.h"

#include "tia/members.h"
#include "tia/session.h"
#include "tia/simaticml.h"
#include "tia/tia_dyn.h"
#include "tia/tia_sw.h"
#include "util/fs.h"
#include "util/strbuf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WATCH_ENTRY "SW.WatchAndForceTables.PlcWatchTableEntry"
#define FORCE_ENTRY "SW.WatchAndForceTables.PlcForceTableEntry"

typedef struct table_ref {
    nav_plc plc;
    sw_found t;
    int force;
} table_ref;

static int is_force_target(tool_ctx *c)
{
    const char *t = arg_s(c, "target");
    if (t && _stricmp(t, "force") == 0)
        return 1;
    return !arg_s(c, "watchTableName") && arg_s(c, "forceTableName");
}

static int resolve_table(tool_ctx *c, table_ref *r, int allow_force)
{
    memset(r, 0, sizeof *r);
    if (sw_plc(c, &r->plc) != 0)
        return -1;
    r->force = allow_force && is_force_target(c);
    const char *name = arg_s(c, r->force ? "forceTableName" : "watchTableName");
    if (!name || !*name)
        name = arg_s(c, "tableName");
    if ((!name || !*name) && r->force)
        name = "Force table";
    if (!name || !*name)
        return fail(c, "missing required argument '%s'", r->force ? "forceTableName" : "watchTableName");
    snprintf(c->error_context, sizeof c->error_context, "device=%s table=%s", r->plc.device_name, name);
    if (sw_find(c, r->plc.software, SWC_WATCH_TABLES, name, &r->t) != 0)
        return -1;
    int is_force = strstr(r->t.type, "Force") != NULL;
    if (r->force != is_force && arg_s(c, "target"))
        return fail(c, "'%s' is a %s table but target=%s", r->t.name, is_force ? "force" : "watch", arg_s(c, "target"));
    r->force = is_force;
    return 0;
}

static const char *collection_of(const table_ref *r)
{
    return r->force ? "ForceTables" : "WatchTables";
}

/* "%M60.0" and "\"Tag\"" stay; Tag -> "Tag"; DB.member -> "DB".member. */
static void normalize_operand(const char *in, char *out, size_t cap)
{
    while (isspace((unsigned char)*in))
        in++;
    if (*in == '%' || *in == '"' || *in == '#' || !*in) {
        snprintf(out, cap, "%s", in);
        return;
    }
    const char *dot = strchr(in, '.');
    if (dot)
        snprintf(out, cap, "\"%.*s\"%s", (int)(dot - in), in, dot);
    else
        snprintf(out, cap, "\"%s\"", in);
}

static const char *entry_element(int force)
{
    return force ? FORCE_ENTRY : WATCH_ENTRY;
}

static mxml_node_t *entries_list(mxml_node_t *obj, int create)
{
    mxml_node_t *ol = sml_child(obj, "ObjectList");
    if (!ol && create)
        ol = mxmlNewElement(obj, "ObjectList");
    return ol;
}

/* Symbolic operands are stored in Name, absolute ones in Address. */
static const char *entry_name(mxml_node_t *e)
{
    mxml_node_t *al = sml_child(e, "AttributeList");
    const char *n = sml_child_text(al, "Name");
    if (n && *n)
        return n;
    n = sml_child_text(al, "Address");
    return n && *n ? n : NULL;
}

/* Finds an entry by operand; for an absolute address also by the tag that owns it. */
static mxml_node_t *find_entry(tool_ctx *c, table_ref *r, mxml_node_t *obj, const char *address)
{
    char want[512];
    normalize_operand(address, want, sizeof want);
    mxml_node_t *ol = entries_list(obj, 0);
    for (mxml_node_t *e = sml_child(ol, entry_element(r->force)); e; e = sml_next(e, entry_element(r->force))) {
        const char *n = entry_name(e);
        if (!n)
            continue;
        char base[512];
        snprintf(base, sizeof base, "%s", n);
        char *colon = strstr(base, ":P");
        if (colon && colon[2] == 0)
            *colon = 0; /* peripheral suffix */
        if (_stricmp(n, want) == 0 || _stricmp(base, want) == 0)
            return e;
    }
    (void)c;
    return NULL;
}

static void set_attr_text(mxml_node_t *al, const char *name, const char *value)
{
    if (!value)
        return;
    mxml_node_t *n = sml_child(al, name);
    if (!*value) {
        if (n)
            mxmlDelete(n);
        return;
    }
    if (!n) {
        /* Keep the alphabetical order used by TIA exports. */
        mxml_node_t *before = NULL;
        for (mxml_node_t *x = sml_child(al, NULL); x; x = sml_next(x, NULL))
            if (strcmp(mxmlGetElement(x), name) > 0) {
                before = x;
                break;
            }
        n = mxmlNewElement(NULL, name);
        if (before)
            mxmlAdd(al, MXML_ADD_BEFORE, before, n);
        else
            mxmlAdd(al, MXML_ADD_AFTER, NULL, n);
    }
    sml_set_text(n, value);
}

static int max_id(mxml_node_t *top)
{
    int max = 0;
    for (mxml_node_t *n = top; n; n = mxmlWalkNext(n, top, MXML_DESCEND_ALL)) {
        const char *id = mxmlGetType(n) == MXML_TYPE_ELEMENT ? mxmlElementGetAttr(n, "ID") : NULL;
        if (id && (int)strtol(id, NULL, 16) > max)
            max = (int)strtol(id, NULL, 16);
    }
    return max;
}

static void set_entry_comment(mxml_node_t *e, const char *culture, const char *text, int *next_id)
{
    if (!text)
        return;
    mxml_node_t *ol = sml_ensure_child(e, "ObjectList", 0);
    mxml_node_t *mt = sml_child(ol, "MultilingualText");
    if (!mt) {
        mt = mxmlNewElement(ol, "MultilingualText");
        mxmlElementSetAttrf(mt, "ID", "%X", (*next_id)++);
        mxmlElementSetAttr(mt, "CompositionName", "Comment");
    }
    mxml_node_t *items = sml_ensure_child(mt, "ObjectList", 0);
    mxml_node_t *item;
    for (item = sml_child(items, "MultilingualTextItem"); item; item = sml_next(item, "MultilingualTextItem")) {
        const char *cul = sml_child_text(sml_child(item, "AttributeList"), "Culture");
        if (cul && _stricmp(cul, culture) == 0)
            break;
    }
    if (!item) {
        item = mxmlNewElement(items, "MultilingualTextItem");
        mxmlElementSetAttrf(item, "ID", "%X", (*next_id)++);
        mxmlElementSetAttr(item, "CompositionName", "Items");
        mxml_node_t *al = mxmlNewElement(item, "AttributeList");
        mxmlNewOpaque(mxmlNewElement(al, "Culture"), culture);
        mxmlNewElement(al, "Text");
    }
    sml_set_text(sml_ensure_child(sml_child(item, "AttributeList"), "Text", 0), text);
}

static const char *entry_comment(mxml_node_t *e)
{
    mxml_node_t *mt = sml_child(sml_child(e, "ObjectList"), "MultilingualText");
    for (mxml_node_t *it = sml_child(sml_child(mt, "ObjectList"), "MultilingualTextItem"); it;
         it = sml_next(it, "MultilingualTextItem")) {
        const char *t = sml_child_text(sml_child(it, "AttributeList"), "Text");
        if (t && *t)
            return t;
    }
    return NULL;
}

/* Applies the entry arguments (display format, values, triggers, comment). */
static void apply_entry_args(tool_ctx *c, table_ref *r, mxml_node_t *e, const char *culture, int *next_id, int is_new)
{
    mxml_node_t *al = sml_ensure_child(e, "AttributeList", 1);
    const char *fmt = arg_s(c, "displayFormat");
    set_attr_text(al, "DisplayFormat", fmt ? fmt : (is_new ? "Undef" : NULL));
    if (r->force) {
        set_attr_text(al, "ForceValue", arg_s(c, "forceValue"));
    } else {
        set_attr_text(al, "ModifyValue", arg_s(c, "modifyValue"));
        set_attr_text(al, "ModifyTrigger", arg_s(c, "modifyTrigger"));
    }
    const char *mt = arg_s(c, "monitorTrigger");
    set_attr_text(al, "MonitorTrigger", mt ? mt : (is_new ? "Permanent" : NULL));
    set_entry_comment(e, culture, arg_s(c, "comment"), next_id);
}

static int commit(tool_ctx *c, table_ref *r, mxml_node_t *top, const char *msg)
{
    tool_ctx probe;
    ctx_init(&probe, c->tool, NULL);
    th res = mb_reimport(&probe, r->t.item, collection_of(r), top);
    if (!res && strstr(sb_str(&probe.out), "inconsistent")) {
        /* Entries referring to objects that were never compiled are refused: compile and retry once. */
        tool_ctx quiet;
        ctx_init(&quiet, c->tool, NULL);
        compile_object(&quiet, r->plc.software, 1);
        ctx_free(&quiet);
        sb_clear(&probe.out);
        probe.is_error = 0;
        res = mb_reimport(&probe, r->t.item, collection_of(r), top);
        if (res)
            out(c, "(compiled the PLC software first: a referenced object was inconsistent)\n");
    }
    if (!res) {
        out_raw(c, sb_str(&probe.out));
        c->is_error = 1;
    }
    ctx_free(&probe);
    mxmlDelete(top);
    if (!res)
        return -1;
    out(c, "%s\n", msg);
    return 0;
}

/* ---- tables ----------------------------------------------------------------------- */

typedef struct list_ctx {
    tool_ctx *c;
    int n;
} list_ctx;

static int list_cb(void *ctx, const cJSON *item, const char *folder)
{
    list_ctx *l = ctx;
    print_item(l->c, SWC_WATCH_TABLES, item, folder);
    l->n++;
    return 0;
}

static int a_list_tables(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    list_ctx l = { c, 0 };
    sw_walk(plc.software, SWC_WATCH_TABLES, "Name", list_cb, NULL, &l);
    out(c, "Total: %d table(s). Entry counts include comment rows.\n", l.n);
    return 0;
}

static int a_create_table(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *name = arg_req(c, "watchTableName");
    if (!name)
        return -1;
    tool_ctx probe;
    ctx_init(&probe, c->tool, NULL);
    sw_found ex;
    int exists = sw_find(&probe, plc.software, SWC_WATCH_TABLES, name, &ex) == 0;
    ctx_free(&probe);
    if (exists)
        return fail(c, "table '%s' already exists", name);
    th group = sw_folder(c, plc.software, SWC_WATCH_TABLES, arg_s(c, "targetFolder"), 1, arg_b(c, "createParents", 0));
    th tables = group ? td_get_h(group, "WatchTables") : 0;
    if (!tables)
        return -1;
    if (!td_call_h(tables, "Create", tda("s", name)))
        return fail_td(c, "creating the watch table failed");
    out(c, "Watch table '%s' created.\n", name);
    return 0;
}

static int a_delete_table(tool_ctx *c)
{
    table_ref r;
    if (resolve_table(c, &r, 0) != 0)
        return -1;
    if (r.force)
        return fail(c, "the force table cannot be deleted (one per PLC): use clear_table semantics with delete_entry");
    if (td_call_v(r.t.item, "Delete", NULL) != 0)
        return fail_td(c, "delete failed");
    out(c, "Watch table '%s' deleted.\n", r.t.name);
    return 0;
}

static int a_rename_table(tool_ctx *c)
{
    table_ref r;
    if (resolve_table(c, &r, 0) != 0)
        return -1;
    const char *nn = arg_req(c, "newName");
    if (!nn)
        return -1;
    if (r.force)
        return fail(c, "the force table cannot be renamed");
    if (td_set(r.t.item, "Name", cJSON_CreateString(nn)) == 0) {
        out(c, "Watch table '%s' renamed to '%s'.\n", r.t.name, nn);
        return 0;
    }
    /* Fallback: XML round trip with the new name. */
    td_clear_err();
    mxml_node_t *top = sw_export_tree(c, r.t.item);
    if (!top)
        return -1;
    sml_set_text(sml_child(sml_attribute_list(sml_object(top)), "Name"), nn);
    th parent = td_get_h(r.t.item, "Parent");
    th res = mb_import_tree(c, parent, "WatchTables", top, 0);
    mxmlDelete(top);
    if (!res)
        return -1;
    td_call_v(r.t.item, "Delete", NULL);
    out(c, "Watch table '%s' renamed to '%s' (XML round trip).\n", r.t.name, nn);
    return 0;
}

static int a_move_table(tool_ctx *c)
{
    table_ref r;
    if (resolve_table(c, &r, 0) != 0)
        return -1;
    if (r.force)
        return fail(c, "the force table cannot be moved");
    if (!arg_has(c, "targetFolder"))
        return fail(c, "missing required argument 'targetFolder' ('' = root)");
    th target = sw_folder(c, r.plc.software, SWC_WATCH_TABLES, arg_s(c, "targetFolder"), 1, arg_b(c, "createParents", 0));
    if (!target || !mb_move(c, r.t.item, "WatchTables", target))
        return -1;
    out(c, "Watch table '%s' moved to '%s'.\n", r.t.name, *arg_s(c, "targetFolder") ? arg_s(c, "targetFolder") : "(root)");
    return 0;
}

/* ---- entries ---------------------------------------------------------------------- */

static int a_get_entries(tool_ctx *c)
{
    table_ref r;
    if (resolve_table(c, &r, 1) != 0)
        return -1;
    mxml_node_t *top = sw_export_tree(c, r.t.item);
    if (!top)
        return -1;
    mxml_node_t *ol = entries_list(sml_object(top), 0);
    int n = 0;
    out(c, "%s table '%s':\n", r.force ? "Force" : "Watch", r.t.name);
    for (mxml_node_t *e = sml_child(ol, NULL); e; e = sml_next(e, NULL)) {
        const char *kind = mxmlGetElement(e);
        mxml_node_t *al = sml_child(e, "AttributeList");
        if (strstr(kind, "CommentEntry")) {
            const char *cm = entry_comment(e);
            out(c, "  // %s\n", cm ? cm : "");
            continue;
        }
        const char *val = sml_child_text(al, r.force ? "ForceValue" : "ModifyValue");
        const char *mtrig = sml_child_text(al, "ModifyTrigger");
        const char *cm = entry_comment(e);
        out(c, "%s  [format=%s, monitor=%s", entry_name(e) ? entry_name(e) : "?",
            sml_child_text(al, "DisplayFormat") ? sml_child_text(al, "DisplayFormat") : "?",
            sml_child_text(al, "MonitorTrigger") ? sml_child_text(al, "MonitorTrigger") : "?");
        if (val && *val)
            out(c, ", %s=%s", r.force ? "force" : "modify", val);
        if (mtrig && *mtrig)
            out(c, ", modifyTrigger=%s", mtrig);
        if (cm)
            out(c, ", comment=%s", cm);
        out(c, "]\n");
        n++;
    }
    out(c, "%d entr%s.\n", n, n == 1 ? "y" : "ies");
    mxmlDelete(top);
    return 0;
}

static int a_add_entry(tool_ctx *c)
{
    table_ref r;
    if (resolve_table(c, &r, 1) != 0)
        return -1;
    const char *address = arg_req(c, "address");
    if (!address)
        return -1;
    if (r.force && arg_has(c, "modifyValue"))
        return fail(c, "force tables use forceValue, not modifyValue");
    if (!r.force && arg_has(c, "forceValue"))
        return fail(c, "watch tables use modifyValue, not forceValue");
    mxml_node_t *top = sw_export_tree(c, r.t.item);
    if (!top)
        return -1;
    mxml_node_t *obj = sml_object(top);
    if (find_entry(c, &r, obj, address)) {
        mxmlDelete(top);
        return fail(c, "'%s' is already in table '%s': use update_entry", address, r.t.name);
    }
    char op[512];
    normalize_operand(address, op, sizeof op);
    int next = max_id(top) + 1;
    mxml_node_t *e = mxmlNewElement(entries_list(obj, 1), entry_element(r.force));
    mxmlElementSetAttrf(e, "ID", "%X", next++);
    mxmlElementSetAttr(e, "CompositionName", "Entries");
    mxml_node_t *al = mxmlNewElement(e, "AttributeList");
    set_attr_text(al, "Name", op);
    apply_entry_args(c, &r, e, mb_editing_language(session_project()), &next, 1);
    char msg[700];
    snprintf(msg, sizeof msg, "Entry %s added to %s table '%s'.%s", op, r.force ? "force" : "watch", r.t.name,
             arg_has(c, "displayFormat") ? "" : " Tip: set displayFormat to match the data type (Bool, DEC_signed, Hex, Float, ...).");
    return commit(c, &r, top, msg);
}

static int a_update_entry(tool_ctx *c)
{
    table_ref r;
    if (resolve_table(c, &r, 1) != 0)
        return -1;
    const char *address = arg_req(c, "address");
    if (!address)
        return -1;
    mxml_node_t *top = sw_export_tree(c, r.t.item);
    if (!top)
        return -1;
    mxml_node_t *e = find_entry(c, &r, sml_object(top), address);
    if (!e) {
        mxmlDelete(top);
        return fail(c, "'%s' is not in table '%s' (get_entries lists the stored operands)", address, r.t.name);
    }
    int next = max_id(top) + 1;
    apply_entry_args(c, &r, e, mb_editing_language(session_project()), &next, 0);
    char msg[600];
    snprintf(msg, sizeof msg, "Entry %s of '%s' updated.", entry_name(e) ? entry_name(e) : address, r.t.name);
    return commit(c, &r, top, msg);
}

static int a_delete_entry(tool_ctx *c)
{
    table_ref r;
    if (resolve_table(c, &r, 1) != 0)
        return -1;
    const char *address = arg_req(c, "address");
    if (!address)
        return -1;
    mxml_node_t *top = sw_export_tree(c, r.t.item);
    if (!top)
        return -1;
    mxml_node_t *e = find_entry(c, &r, sml_object(top), address);
    if (!e) {
        mxmlDelete(top);
        return fail(c, "'%s' is not in table '%s' (get_entries lists the stored operands)", address, r.t.name);
    }
    mxmlDelete(e);
    char msg[600];
    snprintf(msg, sizeof msg, "Entry %s removed from '%s'.", address, r.t.name);
    return commit(c, &r, top, msg);
}

static int a_clear_table(tool_ctx *c)
{
    table_ref r;
    if (resolve_table(c, &r, 1) != 0)
        return -1;
    mxml_node_t *top = sw_export_tree(c, r.t.item);
    if (!top)
        return -1;
    mxml_node_t *ol = entries_list(sml_object(top), 0);
    int n = 0;
    if (ol) {
        mxmlDelete(ol);
        n = 1;
    }
    if (!n) {
        mxmlDelete(top);
        out(c, "Table '%s' is already empty.\n", r.t.name);
        return 0;
    }
    char msg[400];
    snprintf(msg, sizeof msg, "All entries of '%s' removed (table kept).", r.t.name);
    return commit(c, &r, top, msg);
}

/* ---- export / import ------------------------------------------------------------------ */

static int a_export_table(tool_ctx *c)
{
    table_ref r;
    if (resolve_table(c, &r, 1) != 0)
        return -1;
    const char *outp = arg_req(c, "outputPath");
    if (!outp)
        return -1;
    char path[1024], name[300];
    if (sw_export_xml(c, r.t.item, path, sizeof path) != 0)
        return -1;
    snprintf(name, sizeof name, "%s.xml", r.t.name);
    int rc = sw_deliver(c, path, name, "application/xml", outp, 0);
    fs_remove(path);
    return rc;
}

static int export_data(tool_ctx *c, int force)
{
    table_ref r;
    if (resolve_table(c, &r, 1) != 0)
        return -1;
    if (r.force != force)
        return fail(c, "'%s' is a %s table: use %s", r.t.name, r.force ? "force" : "watch",
                    r.force ? "export_force_data" : "export_watch_data");
    int xlsx;
    if (table_format(c, &xlsx) != 0)
        return -1;
    mxml_node_t *top = sw_export_tree(c, r.t.item);
    if (!top)
        return -1;
    strbuf sb;
    sb_init(&sb);
    sb_printf(&sb, "\xEF\xBB\xBFName;DisplayFormat;%s;MonitorTrigger;%sComment\r\n", force ? "ForceValue" : "ModifyValue",
              force ? "" : "ModifyTrigger;");
    mxml_node_t *ol = entries_list(sml_object(top), 0);
    for (mxml_node_t *e = sml_child(ol, entry_element(force)); e; e = sml_next(e, entry_element(force))) {
        mxml_node_t *al = sml_child(e, "AttributeList");
        csv_field(&sb, entry_name(e));
        sb_appendc(&sb, ';');
        csv_field(&sb, sml_child_text(al, "DisplayFormat"));
        sb_appendc(&sb, ';');
        csv_field(&sb, sml_child_text(al, force ? "ForceValue" : "ModifyValue"));
        sb_appendc(&sb, ';');
        csv_field(&sb, sml_child_text(al, "MonitorTrigger"));
        sb_appendc(&sb, ';');
        if (!force) {
            csv_field(&sb, sml_child_text(al, "ModifyTrigger"));
            sb_appendc(&sb, ';');
        }
        csv_field(&sb, entry_comment(e));
        sb_append(&sb, "\r\n");
    }
    mxmlDelete(top);
    int rc = deliver_table(c, &sb, r.t.name, force ? "Force table" : "Watch table", xlsx);
    sb_free(&sb);
    return rc;
}

static int a_export_watch_data(tool_ctx *c) { return export_data(c, 0); }
static int a_export_force_data(tool_ctx *c) { return export_data(c, 1); }

static int a_import_table(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    char path[1024];
    int temp = 0;
    const char *file = arg_s(c, "filePath");
    const char *xml = arg_s(c, "xmlContent");
    if (file && *file) {
        if (fs_full_path(file, path, sizeof path) != 0 || !fs_is_file(path))
            return fail(c, "file '%s' not found", file);
    } else if (xml && *xml) {
        if (fs_temp_path("inline", ".xml", path, sizeof path) != 0 || fs_write_all(path, xml, strlen(xml)) != 0)
            return fail(c, "cannot write a temporary file");
        temp = 1;
    } else {
        return fail(c, "pass filePath or xmlContent");
    }
    char err[256];
    mxml_node_t *top = sml_load_file(path, err, sizeof err);
    char kind[128];
    snprintf(kind, sizeof kind, "%s", top && sml_object(top) ? mxmlGetElement(sml_object(top)) : err);
    int force = strstr(kind, "PlcForceTable") != NULL;
    int watch = strstr(kind, "PlcWatchTable") != NULL;
    if (top)
        mxmlDelete(top);
    if (!force && !watch) {
        if (temp)
            fs_remove(path);
        return fail(c, "the XML does not contain a watch or force table (%s)", kind);
    }
    th group = sw_folder(c, plc.software, SWC_WATCH_TABLES, arg_s(c, "targetFolder"), 1, arg_b(c, "createParents", 0));
    th res = group ? mb_import_file(c, group, force ? "ForceTables" : "WatchTables", path, force || arg_b(c, "overwrite", 0)) : 0;
    if (temp)
        fs_remove(path);
    if (!res)
        return -1;
    char *name = td_get_s(res, "Name");
    out(c, "%s table '%s' imported%s.\n", force ? "Force" : "Watch", name ? name : "?",
        force || arg_b(c, "overwrite", 0) ? " (whole-object replace)" : "");
    free(name);
    return 0;
}

#define W (AF_PROJECT | AF_WRITES)

static const action_def actions[] = {
    { "add_entry",
      "deviceName, target=watch|force, tableName, address; optional displayFormat, modifyValue (watch), forceValue (force), "
      "modifyTrigger (watch), monitorTrigger, comment",
      "Add an entry. address: absolute (%M60.0, %MW2) or symbolic (Tag, DB.member - quoted automatically). ALWAYS set "
      "displayFormat to match the variable: Bool, DEC_signed, DEC_unsigned, Hex, Bin, Float, Character, String, Time, "
      "...; triggers: Permanent, PermanentAtStart, OnceOnlyAtStart, ...",
      a_add_entry, W },
    { "clear_table", "deviceName, watchTableName", "Remove all entries, keep the table.", a_clear_table, W },
    { "create_table", "deviceName, watchTableName; optional targetFolder, createParents=false",
      "New empty watch table (folder 'A/B' must exist unless createParents=true).", a_create_table, W },
    { "delete_entry", "deviceName, target, watchTableName|forceTableName, address",
      "Remove an entry. Pass the absolute address or the exact symbolic name (get_entries shows the stored form).",
      a_delete_entry, W },
    { "delete_table", "deviceName, watchTableName", "Delete a watch table.", a_delete_table, W | AF_DESTRUCTIVE },
    { "export_force_data", "deviceName, forceTableName; optional format=csv|xlsx, outputPath",
      "Force table entries as CSV (inline or to outputPath) or Excel.", a_export_force_data, AF_PROJECT },
    { "export_table", "deviceName, target, tableName, outputPath", "Export a watch/force table to SimaticML XML.",
      a_export_table, AF_PROJECT },
    { "export_watch_data", "deviceName, watchTableName; optional format=csv|xlsx, outputPath",
      "Watch table entries as CSV (inline or to outputPath) or Excel.", a_export_watch_data, AF_PROJECT },
    { "get_entries", "deviceName, target, watchTableName|forceTableName",
      "List entries (operand, display format, modify/force value, triggers, comment; comment rows as //).", a_get_entries,
      AF_PROJECT },
    { "import_table", "deviceName, filePath OR xmlContent; optional overwrite, targetFolder, createParents",
      "Import a watch or force table from XML. OVERWRITE IS A WHOLE-OBJECT REPLACE, NOT A MERGE (the force table is always "
      "replaced).",
      a_import_table, W },
    { "list_tables", "deviceName",
      "All watch and force tables: 'Folder/Name  [type=PlcWatchTable|PlcForceTable, entries=N, force=true|false]'.",
      a_list_tables, AF_PROJECT },
    { "move_table", "deviceName, watchTableName, targetFolder; optional createParents=false",
      "OFFLINE REQUIRED. Move a watch table between folders ('' = root), restored on failure.", a_move_table,
      W | AF_OFFLINE },
    { "rename_table", "deviceName, watchTableName, newName", "Rename a watch table.", a_rename_table, W },
    { "update_entry", "same as add_entry",
      "Update an existing entry (fields not passed are kept; an empty string clears values/comment).", a_update_entry, W },
};

const tool_def tool_watch = {
    .name = "watch",
    .title = "Watch & force tables",
    .summary = "Watch and force tables of a PLC: list, create, rename, move, delete, entries (add/update/delete/clear), "
               "CSV/XML export and XML import. Monitoring live values needs TIA Portal online (live data is not part of "
               "this server).",
    .properties =
        "{"
        "\"deviceName\":{\"type\":\"string\",\"description\":\"Device or PLC name (session list_devices).\"},"
        "\"target\":{\"type\":\"string\",\"enum\":[\"watch\",\"force\"],\"description\":\"Table kind (default watch).\"},"
        "\"tableName\":{\"type\":\"string\"},\"watchTableName\":{\"type\":\"string\"},\"forceTableName\":{\"type\":\"string\"},"
        "\"address\":{\"type\":\"string\",\"description\":\"%M60.0, %MW2, Tag or DB.member\"},"
        "\"displayFormat\":{\"type\":\"string\",\"description\":\"Bool, DEC_signed, DEC_unsigned, Hex, Bin, Float, Character, String, Time, DATE_AND_TIME, ...\"},"
        "\"modifyValue\":{\"type\":\"string\"},\"forceValue\":{\"type\":\"string\"},"
        "\"modifyTrigger\":{\"type\":\"string\"},\"monitorTrigger\":{\"type\":\"string\"},\"comment\":{\"type\":\"string\"},"
        "\"newName\":{\"type\":\"string\"},\"targetFolder\":{\"type\":\"string\"},\"createParents\":{\"type\":\"boolean\"},"
        "\"filePath\":{\"type\":\"string\"},\"xmlContent\":{\"type\":\"string\"},\"overwrite\":{\"type\":\"boolean\"},"
        "\"outputPath\":{\"type\":\"string\"},\"format\":{\"type\":\"string\",\"enum\":[\"csv\",\"xlsx\"]}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
};
