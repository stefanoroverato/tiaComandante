/* blocks_read: list, inspect, export and compile program blocks. */
#include "tools.h"

#include "app/config.h"
#include "tia/simaticml.h"
#include "tia/tia_dyn.h"
#include "tia/tia_sw.h"
#include "util/fs.h"
#include "util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_ATTRS "Name,Number,ProgrammingLanguage,IsConsistent"

/* ---- shared ------------------------------------------------------------------ */

static int is_db_type(const char *t)
{
    return strstr(t, "DB") != NULL || strcmp(t, "DataBlock") == 0;
}

static int resolve_block(tool_ctx *c, nav_plc *plc, sw_found *blk)
{
    if (sw_plc(c, plc) != 0)
        return -1;
    const char *name = arg_req(c, "blockName");
    if (!name)
        return -1;
    snprintf(c->error_context, sizeof c->error_context, "device=%s block=%s", plc->device_name, name);
    return sw_find(c, plc->software, SWC_BLOCKS, name, blk);
}

static void block_label(const sw_found *b, long long number, char *out, size_t cap)
{
    const char *t = b->type;
    const char *prefix = strcmp(t, "GlobalDB") == 0 || strcmp(t, "InstanceDB") == 0 || strcmp(t, "ArrayDB") == 0 ? "DB" : t;
    snprintf(out, cap, "%s%lld", prefix, number);
}

/* ---- list ---------------------------------------------------------------------- */

typedef struct list_ctx {
    tool_ctx *c;
    const char *filter;
    int include_idb;
    int count;
} list_ctx;

static int filter_match(const char *filter, const char *type)
{
    if (!filter || !*filter || _stricmp(filter, "all") == 0)
        return 1;
    if (_stricmp(filter, "code") == 0)
        return strcmp(type, "OB") == 0 || strcmp(type, "FB") == 0 || strcmp(type, "FC") == 0;
    if (_stricmp(filter, "DB") == 0)
        return is_db_type(type);
    return _stricmp(filter, type) == 0;
}

static int list_cb(void *ctx, const cJSON *item, const char *folder)
{
    list_ctx *l = ctx;
    const char *type = short_type(item);
    if (!filter_match(l->filter, type))
        return 0;
    if (!l->include_idb && strcmp(type, "InstanceDB") == 0 && (!l->filter || _stricmp(l->filter, "InstanceDB") != 0))
        return 0;
    char *num = item_text(item, "Number");
    out(l->c, "%s%s%s  [type=%s, num=%s, lang=%s, consistent=%s", folder, *folder ? "/" : "", tdi_s(item, "Name"), type,
        num, tdi_s(item, "ProgrammingLanguage") ? tdi_s(item, "ProgrammingLanguage") : "?",
        tdi_b(item, "IsConsistent", 0) ? "true" : "false");
    if (strcmp(type, "InstanceDB") == 0) {
        char *of = td_get_s(tdv_h(item), "InstanceOfName");
        if (of)
            out(l->c, ", instanceOf=%s", of);
        free(of);
    }
    out(l->c, "]\n");
    free(num);
    l->count++;
    return 0;
}

static int a_list(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    list_ctx l = { c, arg_s(c, "typeFilter"), arg_b(c, "includeInstanceDbs", 1), 0 };
    out(c, "Blocks of %s:\n", plc.device_name);
    if (sw_walk(plc.software, SWC_BLOCKS, BLOCK_ATTRS, list_cb, NULL, &l) != 0 && td_failed())
        return fail_td(c, "reading the blocks failed");
    out(c, "Total: %d block(s)\n", l.count);
    return 0;
}

/* ---- details / interface ---------------------------------------------------------- */

typedef struct section_count {
    char name[32];
    int members;
} section_count;

typedef struct count_ctx {
    section_count s[16];
    int n;
} count_ctx;

static int count_cb(void *ctx, mxml_node_t *member, const char *section, const char *path, int depth)
{
    (void)member;
    (void)path;
    count_ctx *k = ctx;
    if (depth != 0)
        return 0;
    for (int i = 0; i < k->n; i++) {
        if (strcmp(k->s[i].name, section) == 0) {
            k->s[i].members++;
            return 0;
        }
    }
    if (k->n < 16) {
        snprintf(k->s[k->n].name, sizeof k->s[k->n].name, "%s", section);
        k->s[k->n].members = 1;
        k->n++;
    }
    return 0;
}

static void count_sections(mxml_node_t *sections, count_ctx *k)
{
    memset(k, 0, sizeof *k);
    /* Report empty sections too. */
    for (mxml_node_t *s = sml_child(sections, "Section"); s && k->n < 16; s = sml_next(s, "Section")) {
        snprintf(k->s[k->n].name, sizeof k->s[k->n].name, "%s", sml_attr(s, "Name") ? sml_attr(s, "Name") : "?");
        k->n++;
    }
    sml_walk_members(sections, 0, count_cb, k);
}

static void print_counts(tool_ctx *c, const count_ctx *k)
{
    for (int i = 0; i < k->n; i++)
        out(c, "%s%s=%d", i ? ", " : "", k->s[i].name, k->s[i].members);
}

static void ml_text_of(th obj, const char *prop, char *out_buf, size_t cap)
{
    out_buf[0] = 0;
    th mt = td_get_h(obj, prop);
    th items = mt ? td_get_h(mt, "Items") : 0;
    cJSON *list = items ? td_enum(items, "Text", -1) : NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        const char *t = tdi_s(it, "Text");
        if (t && *t) {
            snprintf(out_buf, cap, "%s", t);
            break;
        }
    }
    cJSON_Delete(list);
}

static int a_get_details(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve_block(c, &plc, &b) != 0)
        return -1;
    cJSON *a = td_attrs(b.item, "Number,ProgrammingLanguage,IsConsistent,IsKnowHowProtected,MemoryLayout,HeaderAuthor,"
                                "HeaderFamily,HeaderName,HeaderVersion,AutoNumber,CreationDate,ModifiedDate,"
                                "InterfaceModifiedDate,CodeModifiedDate,CompileDate,LoadMemoryLength,WorkMemoryLength");
    long long number = tdv_i(cJSON_GetObjectItemCaseSensitive(a, "Number"), 0);
    char label[64];
    block_label(&b, number, label, sizeof label);
    out(c, "Block: %s (%s)\nType: %s\nFolder: %s\n", b.name, label, b.type, *b.folder ? b.folder : "(root)");
    const cJSON *v;
    cJSON_ArrayForEach(v, a)
    {
        if (strcmp(v->string, "Number") == 0 || tdv_is_err(v) || cJSON_IsNull(v))
            continue;
        char *t = tdv_text(v);
        if (*t)
            out(c, "%s: %s\n", v->string, t);
        free(t);
    }
    cJSON_Delete(a);
    if (strcmp(b.type, "InstanceDB") == 0) {
        char *of = td_get_s(b.item, "InstanceOfName");
        out(c, "InstanceOfName: %s\n", of ? of : "?");
        free(of);
    }
    char text[1024];
    ml_text_of(b.item, "Title", text, sizeof text);
    if (*text)
        out(c, "Title: %s\n", text);
    ml_text_of(b.item, "Comment", text, sizeof text);
    if (*text)
        out(c, "Comment: %s\n", text);

    mxml_node_t *top = sw_export_tree(c, b.item);
    if (top) {
        mxml_node_t *obj = sml_object(top);
        count_ctx k;
        count_sections(sml_sections(obj), &k);
        out(c, "Interface: ");
        print_counts(c, &k);
        out(c, "\nNetworks: %d\n", sml_compile_unit_count(obj));
        mxmlDelete(top);
    } else {
        /* Export can fail on know-how protected or inconsistent blocks: keep the details. */
        c->is_error = 0;
        out(c, "(interface not available: export failed)\n");
    }
    return 0;
}

typedef struct iface_ctx {
    tool_ctx *c;
    const char *section;
} iface_ctx;

static int iface_cb(void *ctx, mxml_node_t *member, const char *section, const char *path, int depth)
{
    iface_ctx *f = ctx;
    if (!f->section || strcmp(f->section, section) != 0) {
        out(f->c, "%s:\n", section);
        f->section = section;
    }
    const char *name = strrchr(path, '.');
    name = name ? name + 1 : path;
    out(f->c, "%*s%s : %s", 2 + depth * 2, "", name, sml_attr(member, "Datatype") ? sml_attr(member, "Datatype") : "?");
    const char *sv = sml_start_value(member);
    if (sv && *sv)
        out(f->c, " := %s", sv);
    const char *rem = sml_attr(member, "Remanence");
    if (rem && strcmp(rem, "NonRetain") != 0)
        out(f->c, "  {%s}", rem);
    const char *cm = sml_member_comment(member, NULL);
    if (cm && *cm)
        out(f->c, "  // %s", cm);
    out(f->c, "\n");
    return 0;
}

static int print_interface(tool_ctx *c, mxml_node_t *obj, int max_depth)
{
    mxml_node_t *sections = sml_sections(obj);
    if (!sections) {
        out(c, "(no interface)\n");
        return 0;
    }
    iface_ctx f = { c, NULL };
    /* Empty sections are listed too, so the caller sees what exists. */
    for (mxml_node_t *s = sml_child(sections, "Section"); s; s = sml_next(s, "Section"))
        if (!sml_child(s, "Member"))
            out(c, "%s: (empty)\n", sml_attr(s, "Name") ? sml_attr(s, "Name") : "?");
    sml_walk_members(sections, max_depth, iface_cb, &f);
    return 0;
}

static int a_get_interface(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve_block(c, &plc, &b) != 0)
        return -1;
    mxml_node_t *top = sw_export_tree(c, b.item);
    if (!top) {
        out(c, "Hint: export fails on inconsistent blocks - run blocks_read action=compile_all first.\n");
        return -1;
    }
    out(c, "Interface of %s [%s]:\n", b.name, b.type);
    print_interface(c, sml_object(top), (int)arg_i(c, "depth", -1));
    mxmlDelete(top);
    return 0;
}

typedef struct summary_ctx {
    tool_ctx *c;
    int ok, failed;
} summary_ctx;

static int summary_cb(void *ctx, const cJSON *item, const char *folder)
{
    summary_ctx *s = ctx;
    if (cancelled(s->c))
        return 1;
    const char *name = tdi_s(item, "Name");
    tool_ctx probe;
    ctx_init(&probe, s->c->tool, NULL);
    mxml_node_t *top = sw_export_tree(&probe, tdv_h(item));
    out(s->c, "%s%s%s  [type=%s, num=%lld] ", folder, *folder ? "/" : "", name ? name : "?", short_type(item),
        tdi_i(item, "Number", 0));
    if (top) {
        count_ctx k;
        count_sections(sml_sections(sml_object(top)), &k);
        print_counts(s->c, &k);
        out(s->c, "\n");
        mxmlDelete(top);
        s->ok++;
    } else {
        out(s->c, "(export failed)\n");
        s->failed++;
    }
    ctx_free(&probe);
    progress(s->c, s->ok + s->failed, 0, name);
    return 0;
}

static int a_get_all_interfaces_summary(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    summary_ctx s = { c, 0, 0 };
    out(c, "Interface member counts (top-level members per section):\n");
    sw_walk(plc.software, SWC_BLOCKS, "Name,Number", summary_cb, NULL, &s);
    out(c, "Total: %d block(s)%s", s.ok, s.failed ? "" : "\n");
    if (s.failed)
        out(c, ", %d could not be exported (know-how protected or inconsistent: compile_all first)\n", s.failed);
    return 0;
}

/* ---- export ---------------------------------------------------------------------- */

static const char *output_arg(tool_ctx *c)
{
    const char *p = arg_s(c, "outputPath");
    if (!p || !*p)
        p = arg_s(c, "outputDirectory");
    return p && *p ? p : NULL;
}

static int a_export_xml_file(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve_block(c, &plc, &b) != 0)
        return -1;
    char path[TC_PATH_MAX], name[300];
    if (sw_export_xml(c, b.item, path, sizeof path) != 0)
        return -1;
    snprintf(name, sizeof name, "%s.xml", b.name);
    int rc = sw_deliver(c, path, name, "application/xml", output_arg(c), arg_b(c, "returnInline", 1));
    fs_remove(path);
    return rc;
}

static int a_get_xml_raw(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve_block(c, &plc, &b) != 0)
        return -1;
    char path[TC_PATH_MAX], name[300];
    if (sw_export_xml(c, b.item, path, sizeof path) != 0)
        return -1;
    snprintf(name, sizeof name, "%s.xml", b.name);
    int rc = sw_deliver(c, path, name, "application/xml", NULL, 1);
    fs_remove(path);
    return rc;
}

static void network_line(tool_ctx *c, mxml_node_t *cu, int index)
{
    mxml_node_t *al = sml_child(cu, "AttributeList");
    const char *lang = sml_child_text(al, "ProgrammingLanguage");
    const char *title = sml_ml_text(cu, "Title", NULL);
    out(c, "  [%d] lang=%s%s%s\n", index, lang ? lang : "?", title && *title ? ", title=" : "", title && *title ? title : "");
}

static int a_export_xml_inline(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve_block(c, &plc, &b) != 0)
        return -1;
    mxml_node_t *top = sw_export_tree(c, b.item);
    if (!top)
        return -1;
    mxml_node_t *obj = sml_object(top);
    mxml_node_t *al = sml_attribute_list(obj);
    out(c, "%s (%s)\n", b.name, mxmlGetElement(obj));
    static const char *attrs[] = { "Number", "ProgrammingLanguage", "MemoryLayout", "HeaderAuthor", "HeaderFamily",
                                   "HeaderName", "HeaderVersion", "SecondaryType", "InstanceOfName", "AutoNumber" };
    for (size_t i = 0; i < sizeof attrs / sizeof attrs[0]; i++) {
        const char *v = sml_child_text(al, attrs[i]);
        if (v && *v)
            out(c, "%s: %s\n", attrs[i], v);
    }
    const char *title = sml_ml_text(obj, "Title", NULL);
    if (title && *title)
        out(c, "Title: %s\n", title);
    const char *comment = sml_ml_text(obj, "Comment", NULL);
    if (comment && *comment)
        out(c, "Comment: %s\n", comment);
    out(c, "Interface:\n");
    print_interface(c, obj, 1);
    int n = sml_compile_unit_count(obj);
    out(c, "Networks: %d\n", n);
    for (int i = 0; i < n; i++)
        network_line(c, sml_compile_unit(obj, i), i);
    mxmlDelete(top);
    return 0;
}

static const char *source_ext(const char *type, const char *lang)
{
    if (is_db_type(type))
        return ".db";
    if (lang && strcmp(lang, "STL") == 0)
        return ".awl";
    return ".scl";
}

static int a_export_source(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve_block(c, &plc, &b) != 0)
        return -1;
    char *lang = td_get_s(b.item, "ProgrammingLanguage");
    if (lang && (strcmp(lang, "LAD") == 0 || strcmp(lang, "FBD") == 0 || strcmp(lang, "GRAPH") == 0)) {
        fail(c, "%s is a %s block: sources can only be generated for SCL, STL and DB blocks. Use export_xml_file instead.",
             b.name, lang);
        free(lang);
        return -1;
    }
    const char *ext = source_ext(b.type, lang);
    free(lang);
    char path[TC_PATH_MAX];
    if (fs_temp_path("source", ext, path, sizeof path) != 0)
        return fail(c, "cannot create a temporary file");
    cJSON *list = cJSON_CreateArray();
    cJSON *h = cJSON_CreateObject();
    cJSON_AddNumberToObject(h, "$h", (double)b.item);
    cJSON_AddItemToArray(list, h);
    th group = td_get_h(plc.software, "ExternalSourceGroup");
    if (td_call_v(group, "GenerateSource", tda("jfs", list, path, "None")) != 0) {
        fs_remove(path);
        return fail_td(c, "generating the source failed");
    }
    char name[300];
    snprintf(name, sizeof name, "%s%s", b.name, ext);
    int rc = sw_deliver(c, path, name, "text/plain", output_arg(c), arg_b(c, "returnInline", 1));
    fs_remove(path);
    return rc;
}

typedef struct export_all_ctx {
    tool_ctx *c;
    const char *dir;
    int ok, failed;
    strbuf failures;
} export_all_ctx;

static int export_all_cb(void *ctx, const cJSON *item, const char *folder)
{
    export_all_ctx *e = ctx;
    if (cancelled(e->c))
        return 1;
    const char *name = tdi_s(item, "Name");
    char sub[TC_PATH_MAX], target[TC_PATH_MAX], file[300];
    fs_join(sub, sizeof sub, e->dir, folder);
    for (char *p = sub + strlen(e->dir); *p; p++)
        if (*p == '/')
            *p = '\\';
    fs_mkdirs(sub);
    snprintf(file, sizeof file, "%s.xml", name ? name : "block");
    fs_join(target, sizeof target, sub, file);
    fs_remove(target);
    if (td_call_v(tdv_h(item), "Export", tda("fe", target, "Siemens.Engineering.ExportOptions", "WithDefaults")) == 0) {
        e->ok++;
    } else {
        e->failed++;
        if (e->failures.len < 4000)
            sb_printf(&e->failures, "  %s: %s\n", name ? name : "?", td_err());
    }
    progress(e->c, e->ok + e->failed, 0, name);
    return 0;
}

static int a_export_all_xml(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *dir = arg_req(c, "outputDirectory");
    if (!dir)
        return -1;
    char full[TC_PATH_MAX];
    if (fs_full_path(dir, full, sizeof full) != 0 || fs_mkdirs(full) != 0)
        return fail(c, "cannot use output directory '%s'", dir);
    export_all_ctx e = { c, full, 0, 0, { 0 } };
    sb_init(&e.failures);
    sw_walk(plc.software, SWC_BLOCKS, "Name", export_all_cb, NULL, &e);
    out(c, "Exported %d block(s) to %s (folders mirrored).\n", e.ok, full);
    if (e.failed)
        out(c, "%d failed:\n%s", e.failed, sb_str(&e.failures));
    sb_free(&e.failures);
    out(c, "Ask the user whether to open the folder (admin action=open_file).\n");
    return 0;
}

/* ---- compile ----------------------------------------------------------------------- */

static int a_compile_all(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    return compile_object(c, plc.software, 0);
}

static int a_compile_block(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve_block(c, &plc, &b) != 0)
        return -1;
    return compile_object(c, b.item, 0);
}

static int a_get_compiler_messages(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    out(c, "(fresh compilation of the PLC software)\n");
    return compile_object(c, plc.software, 0);
}

static int a_get_compiler_errors(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    out(c, "(fresh compilation of the PLC software, errors only)\n");
    return compile_object(c, plc.software, 1);
}

typedef struct consist_ctx {
    tool_ctx *c;
    int total, bad;
} consist_ctx;

static int consist_cb(void *ctx, const cJSON *item, const char *folder)
{
    consist_ctx *k = ctx;
    k->total++;
    if (!tdi_b(item, "IsConsistent", 1)) {
        k->bad++;
        out(k->c, "%s%s%s  [type=%s]\n", folder, *folder ? "/" : "", tdi_s(item, "Name"), short_type(item));
    }
    return 0;
}

static int a_check_consistency(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    consist_ctx k = { c, 0, 0 };
    out(c, "Inconsistent objects:\n");
    sw_walk(plc.software, SWC_BLOCKS, "Name,IsConsistent", consist_cb, NULL, &k);
    int blocks = k.total, bad_blocks = k.bad;
    sw_walk(plc.software, SWC_TYPES, "Name,IsConsistent", consist_cb, NULL, &k);
    if (!k.bad)
        out(c, "(none)\n");
    out(c, "Blocks: %d of %d inconsistent; PLC data types: %d of %d inconsistent.\n", bad_blocks, blocks, k.bad - bad_blocks,
        k.total - blocks);
    out(c, "Note: IsConsistent can lag behind the last compile - compile_all gives the authoritative result.\n");
    return 0;
}

/* ---- networks ---------------------------------------------------------------------- */

static void flgnet_summary(tool_ctx *c, mxml_node_t *flgnet, int uids)
{
    mxml_node_t *parts = sml_child(flgnet, "Parts");
    int nparts = 0;
    for (mxml_node_t *p = sml_child(parts, NULL); p; p = sml_next(p, NULL)) {
        const char *e = mxmlGetElement(p);
        const char *uid = sml_attr(p, "UId");
        nparts++;
        if (uids)
            out(c, "  #%s ", uid ? uid : "?");
        else
            out(c, "  ");
        if (strcmp(e, "Access") == 0) {
            strbuf sb;
            sb_init(&sb);
            const char *scope = sml_attr(p, "Scope");
            sml_render_access(p, &sb);
            out(c, "operand %s (%s)\n", sb.len ? sb_str(&sb) : "?", scope ? scope : "?");
            sb_free(&sb);
        } else if (strcmp(e, "Part") == 0) {
            const char *name = sml_attr(p, "Name");
            const char *ver = sml_attr(p, "Version");
            out(c, "instruction %s%s%s\n", name ? name : "?", ver ? " V" : "", ver ? ver : "");
        } else if (strcmp(e, "Call") == 0) {
            mxml_node_t *ci = sml_child(p, "CallInfo");
            const char *name = sml_attr(ci, "Name");
            const char *bt = sml_attr(ci, "BlockType");
            mxml_node_t *inst = sml_path(ci, "Instance/Component");
            out(c, "call %s [%s]%s%s\n", name ? name : "?", bt ? bt : "?", inst ? " instance=" : "",
                inst && sml_attr(inst, "Name") ? sml_attr(inst, "Name") : "");
        } else {
            out(c, "%s\n", e);
        }
    }
    int nwires = 0;
    for (mxml_node_t *w = sml_child(sml_child(flgnet, "Wires"), "Wire"); w; w = sml_next(w, "Wire"))
        nwires++;
    out(c, "  (%d parts, %d wires; full graph: blocks_read action=get_xml_raw)\n", nparts, nwires);
}

static int a_get_network(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve_block(c, &plc, &b) != 0)
        return -1;
    if (!arg_has(c, "networkIndex"))
        return fail(c, "missing required argument 'networkIndex' (0-based)");
    int index = (int)arg_i(c, "networkIndex", 0);
    mxml_node_t *top = sw_export_tree(c, b.item);
    if (!top)
        return -1;
    mxml_node_t *obj = sml_object(top);
    int n = sml_compile_unit_count(obj);
    if (index < 0 || index >= n) {
        mxmlDelete(top);
        return fail(c, "networkIndex %d out of range: %s has %d network(s) (0-based)", index, b.name, n);
    }
    mxml_node_t *cu = sml_compile_unit(obj, index);
    mxml_node_t *al = sml_child(cu, "AttributeList");
    const char *lang = sml_child_text(al, "ProgrammingLanguage");
    const char *title = sml_ml_text(cu, "Title", NULL);
    const char *comment = sml_ml_text(cu, "Comment", NULL);
    out(c, "Network %d of %d in %s\nLanguage: %s\nTitle: %s\n", index, n, b.name, lang ? lang : "?", title ? title : "");
    if (comment && *comment)
        out(c, "Comment: %s\n", comment);
    mxml_node_t *src = sml_child(al, "NetworkSource");
    mxml_node_t *st = sml_child(src, "StructuredText");
    mxml_node_t *flg = sml_child(src, "FlgNet");
    if (st) {
        strbuf sb;
        sb_init(&sb);
        sml_render_scl(st, &sb);
        out(c, "SCL:\n%s%s", sb_str(&sb), sb.len && sb.p[sb.len - 1] == '\n' ? "" : "\n");
        sb_free(&sb);
    } else if (flg) {
        out(c, "Elements:\n");
        flgnet_summary(c, flg, arg_b(c, "includeUIds", 0));
    } else if (src) {
        char *xml = sml_save_string(src);
        out(c, "Source XML:\n%s\n", xml ? xml : "");
        free(xml);
    } else {
        out(c, "(empty network)\n");
    }
    mxmlDelete(top);
    return 0;
}

/* ---- table -------------------------------------------------------------------------- */

static const action_def actions[] = {
    { "check_consistency", "deviceName",
      "Blocks and PLC data types whose IsConsistent flag is false. The flag can lag behind compilation: compile_all output "
      "is authoritative.",
      a_check_consistency, AF_PROJECT },
    { "compile_all", "deviceName; optional limit=200",
      "Compile the PLC software (changes) and list errors/warnings. Run early to make exports reliable. First run can take "
      "30-60 s.",
      a_compile_all, AF_PROJECT },
    { "compile_block", "deviceName, blockName", "Compile one block and list its messages.", a_compile_block, AF_PROJECT },
    { "export_all_xml", "deviceName, outputDirectory",
      "Export every block as SimaticML .xml into outputDirectory (argument is 'outputDirectory', not 'outputPath'), "
      "mirroring the folder tree. Afterwards ask the user whether to open the folder (admin action=open_file).",
      a_export_all_xml, AF_PROJECT },
    { "export_source", "deviceName, blockName; optional outputPath, returnInline=true, outputDirectory",
      "SCL/STL/DB source text (generated external source). With outputPath/outputDirectory it is written to disk, "
      "otherwise returned inline (large content goes to the export store). LAD/FBD blocks have no source: use "
      "export_xml_file.",
      a_export_source, AF_PROJECT },
    { "export_xml_file", "deviceName, blockName; optional outputPath, returnInline=true, outputDirectory",
      "Block SimaticML XML. With outputPath it goes to disk, otherwise inline (large content goes to the export store). "
      "After writing a file ask the user whether to open it.",
      a_export_xml_file, AF_PROJECT },
    { "export_xml_inline", "deviceName, blockName",
      "Parsed summary of the block XML: attributes, title/comment, interface (2 levels) and networks.", a_export_xml_inline,
      AF_PROJECT },
    { "get_all_interfaces_summary", "deviceName",
      "Top-level member counts per interface section for all blocks (one export per block).",
      a_get_all_interfaces_summary, AF_PROJECT },
    { "get_compiler_errors", "deviceName; optional limit", "Fresh compile, errors only.", a_get_compiler_errors, AF_PROJECT },
    { "get_compiler_messages", "deviceName; optional limit",
      "Fresh compile with all messages. Prefer the compile_all output if you just compiled.", a_get_compiler_messages,
      AF_PROJECT },
    { "get_details", "deviceName, blockName",
      "Type, number, language, folder, header, memory layout, dates, consistency, title/comment, interface size and "
      "network count.",
      a_get_details, AF_PROJECT },
    { "get_interface", "deviceName, blockName; optional depth",
      "Full interface: sections and members with data type, start value, retain flag and comment (nested structs, UDTs "
      "and multi-instances indented). Fails on inconsistent blocks - compile_all first.",
      a_get_interface, AF_PROJECT },
    { "get_network", "deviceName, blockName, networkIndex; optional includeUIds=false",
      "One network (0-based): language, title, comment and content - SCL statements as text, LAD/FBD as a list of "
      "operands, instructions and calls.",
      a_get_network, AF_PROJECT },
    { "get_xml_raw", "deviceName, blockName", "Raw SimaticML XML string (large content goes to the export store).",
      a_get_xml_raw, AF_PROJECT },
    { "list", "deviceName; optional typeFilter=all|code|OB|FB|FC|DB|GlobalDB|InstanceDB, includeInstanceDbs=true",
      "Compact block table: 'Folder/Name  [type, num, lang, consistent]'.", a_list, AF_PROJECT },
};

const tool_def tool_blocks_read = {
    .name = "blocks_read",
    .title = "Read & export blocks",
    .summary = "Read, inspect, export (SimaticML XML, SCL/STL source) and compile program blocks of a PLC. deviceName "
               "comes from session action=list_devices; blockName may be 'Name' or 'Folder/Sub/Name'.",
    .properties =
        "{"
        "\"deviceName\":{\"type\":\"string\",\"description\":\"Device or PLC name (session list_devices).\"},"
        "\"blockName\":{\"type\":\"string\",\"description\":\"Block name, optionally folder-qualified ('Motors/FB_Motor').\"},"
        "\"typeFilter\":{\"type\":\"string\",\"enum\":[\"all\",\"code\",\"OB\",\"FB\",\"FC\",\"DB\",\"GlobalDB\",\"InstanceDB\"],\"description\":\"list: filter by block type.\"},"
        "\"includeInstanceDbs\":{\"type\":\"boolean\",\"description\":\"list: include instance DBs (default true).\"},"
        "\"outputPath\":{\"type\":\"string\",\"description\":\"Export target file or folder.\"},"
        "\"outputDirectory\":{\"type\":\"string\",\"description\":\"Export target folder (export_all_xml: required).\"},"
        "\"returnInline\":{\"type\":\"boolean\",\"description\":\"Return the content in the response when no output path is given (default true).\"},"
        "\"networkIndex\":{\"type\":\"integer\",\"description\":\"get_network: 0-based network index.\"},"
        "\"includeUIds\":{\"type\":\"boolean\",\"description\":\"get_network: show element UIds.\"},"
        "\"depth\":{\"type\":\"integer\",\"description\":\"get_interface: maximum nesting depth (default unlimited).\"},"
        "\"limit\":{\"type\":\"integer\",\"description\":\"compile actions: maximum messages shown (default 200).\"}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
    .hints = TH_READONLY,
};
