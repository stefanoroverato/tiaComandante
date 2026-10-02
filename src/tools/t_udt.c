/* udt: PLC data types (user-defined types). */
#include "tools.h"

#include "tia/blockgen.h"
#include "tia/members.h"
#include "tia/session.h"
#include "tia/simaticml.h"
#include "tia/tia_dyn.h"
#include "tia/tia_sw.h"
#include "util/fs.h"
#include "util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTION "None"

static int resolve_udt(tool_ctx *c, nav_plc *plc, sw_found *u)
{
    if (sw_plc(c, plc) != 0)
        return -1;
    const char *name = arg_req(c, "udtName");
    if (!name)
        return -1;
    snprintf(c->error_context, sizeof c->error_context, "device=%s udt=%s", plc->device_name, name);
    return sw_find(c, plc->software, SWC_TYPES, name, u);
}

typedef struct list_ctx {
    tool_ctx *c;
    int n;
} list_ctx;

static int list_cb(void *ctx, const cJSON *item, const char *folder)
{
    list_ctx *l = ctx;
    char *mod = item_text(item, "ModifiedDate");
    out(l->c, "%s%s%s  [type=%s, consistent=%s, modified=%s]\n", folder, *folder ? "/" : "", tdi_s(item, "Name"),
        short_type(item), tdi_b(item, "IsConsistent", 1) ? "true" : "false", mod);
    free(mod);
    l->n++;
    return 0;
}

static int a_list(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    list_ctx l = { c, 0 };
    sw_walk(plc.software, SWC_TYPES, "Name,IsConsistent,ModifiedDate", list_cb, NULL, &l);
    out(c, "Total: %d PLC data type(s)\n", l.n);
    return 0;
}

static const char *bool_attr(mxml_node_t *member, const char *name)
{
    mxml_node_t *al = sml_child(member, "AttributeList");
    for (mxml_node_t *a = sml_child(al, "BooleanAttribute"); a; a = sml_next(a, "BooleanAttribute"))
        if (sml_attr(a, "Name") && strcmp(sml_attr(a, "Name"), name) == 0)
            return sml_text(a);
    return NULL;
}

static int struct_cb(void *ctx, mxml_node_t *m, const char *section, const char *path, int depth)
{
    (void)section;
    tool_ctx *c = ctx;
    out(c, "%*s%s : %s", depth * 2, "", sml_attr(m, "Name"), sml_attr(m, "Datatype") ? sml_attr(m, "Datatype") : "?");
    const char *sv = sml_start_value(m);
    if (sv && *sv)
        out(c, " := %s", sv);
    const char *acc = bool_attr(m, "ExternalAccessible");
    const char *vis = bool_attr(m, "ExternalVisible");
    const char *wr = bool_attr(m, "ExternalWritable");
    if ((acc && strcmp(acc, "false") == 0) || (vis && strcmp(vis, "false") == 0) || (wr && strcmp(wr, "false") == 0))
        out(c, "  [accessible=%s, visible=%s, writable=%s]", acc ? acc : "?", vis ? vis : "?", wr ? wr : "?");
    const char *cm = sml_member_comment(m, NULL);
    if (cm && *cm)
        out(c, "  // %s", cm);
    out(c, "\n");
    (void)path;
    return 0;
}

static int a_get_structure(tool_ctx *c)
{
    nav_plc plc;
    sw_found u;
    if (resolve_udt(c, &plc, &u) != 0)
        return -1;
    mxml_node_t *top = sw_export_tree(c, u.item);
    if (!top)
        return -1;
    out(c, "%s [PLC data type]:\n", u.name);
    sml_walk_members(sml_sections(sml_object(top)), -1, struct_cb, c);
    mxmlDelete(top);
    return 0;
}

static int a_export_xml(tool_ctx *c)
{
    nav_plc plc;
    sw_found u;
    if (resolve_udt(c, &plc, &u) != 0)
        return -1;
    char path[1024], name[300];
    if (sw_export_xml(c, u.item, path, sizeof path) != 0)
        return -1;
    snprintf(name, sizeof name, "%s.xml", u.name);
    const char *outp = arg_s(c, "outputPath");
    if (!outp || !*outp)
        outp = arg_s(c, "outputDirectory");
    int rc = sw_deliver(c, path, name, "application/xml", outp && *outp ? outp : NULL, arg_b(c, "returnInline", 1));
    fs_remove(path);
    return rc;
}

/* Writes xmlContent (or uses filePath) and returns the file to import. */
static int import_source(tool_ctx *c, char *path, size_t cap, int *temp)
{
    const char *file = arg_s(c, "filePath");
    const char *xml = arg_s(c, "xmlContent");
    *temp = 0;
    if (file && *file) {
        if (fs_full_path(file, path, cap) != 0 || !fs_is_file(path))
            return fail(c, "file '%s' not found", file);
        return 0;
    }
    if (xml && *xml) {
        if (fs_temp_path("inline", ".xml", path, cap) != 0 || fs_write_all(path, xml, strlen(xml)) != 0)
            return fail(c, "cannot write a temporary file");
        *temp = 1;
        return 0;
    }
    return fail(c, "pass filePath or xmlContent");
}

static int a_import_xml(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    char path[1024];
    int temp = 0;
    if (import_source(c, path, sizeof path, &temp) != 0)
        return -1;
    th group = sw_folder(c, plc.software, SWC_TYPES, arg_s(c, "targetFolder"), 1, arg_b(c, "createParents", 0));
    th res = group ? mb_import_file(c, group, "Types", path, arg_b(c, "overwrite", 0)) : 0;
    if (temp)
        fs_remove(path);
    if (!res) {
        if (!arg_b(c, "overwrite", 0) && strstr(sb_str(&c->out), "exist"))
            out(c, "Hint: pass overwrite=true to replace the existing type (whole-object replace, not a merge).\n");
        return -1;
    }
    char *name = td_get_s(res, "Name");
    out(c, "PLC data type '%s' imported.\n", name ? name : "?");
    free(name);
    return 0;
}

static int a_create(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *name = arg_req(c, "udtName");
    if (!name)
        return -1;
    tool_ctx probe;
    ctx_init(&probe, c->tool, NULL);
    sw_found ex;
    int exists = sw_find(&probe, plc.software, SWC_TYPES, name, &ex) == 0;
    ctx_free(&probe);
    if (exists)
        return fail(c, "PLC data type '%s' already exists", name);
    bg_member *members = NULL;
    int n = 0;
    if (bg_parse_interface(c, arg_get(c, "members"), plc.software, SECTION, &members, &n) != 0)
        return -1;
    bg_member placeholder;
    if (n == 0) {
        /* TIA rejects an empty STRUCT. */
        memset(&placeholder, 0, sizeof placeholder);
        snprintf(placeholder.section, sizeof placeholder.section, SECTION);
        snprintf(placeholder.name, sizeof placeholder.name, "_placeholder");
        snprintf(placeholder.type, sizeof placeholder.type, "Bool");
    }
    for (int i = 0; i < n; i++)
        snprintf(members[i].section, sizeof members[i].section, SECTION);
    th group = sw_folder(c, plc.software, SWC_TYPES, arg_s(c, "targetFolder"), 1, arg_b(c, "createParents", 0));
    if (!group) {
        free(members);
        return -1;
    }
    bg_spec s;
    memset(&s, 0, sizeof s);
    s.kind = "PlcStruct";
    s.name = name;
    s.culture = mb_editing_language(session_project());
    s.members = n ? members : &placeholder;
    s.nmembers = n ? n : 1;
    mxml_node_t *top = bg_build_xml(&s);
    th res = mb_import_tree(c, group, "Types", top, 0);
    mxmlDelete(top);
    free(members);
    if (!res)
        return -1;
    out(c, "PLC data type '%s' created%s.\n", name,
        n ? "" : " with the placeholder member _placeholder (Bool) - remove it with delete_member once real members exist");
    return 0;
}

static int referenced(tool_ctx *c, th item, strbuf *users)
{
    th svc = td_service(item, "Siemens.Engineering.CrossReference.CrossReferenceService");
    th res = svc ? td_call_h(svc, "GetCrossReferences", tda("e", "Siemens.Engineering.CrossReference.CrossReferenceFilter", "AllObjects")) : 0;
    th sources = res ? td_get_h(res, "Sources") : 0;
    (void)c;
    int n = 0;
    cJSON *srcs = sources ? td_enum(sources, NULL, -1) : NULL;
    const cJSON *s;
    cJSON_ArrayForEach(s, srcs)
    {
        cJSON *refs = td_enum(td_get_h(tdv_h(s), "References"), "Name", -1);
        const cJSON *r;
        cJSON_ArrayForEach(r, refs)
        {
            cJSON *locs = td_enum(td_get_h(tdv_h(r), "Locations"), "ReferenceType", -1);
            const cJSON *l;
            cJSON_ArrayForEach(l, locs)
            {
                const char *rt = tdi_s(l, "ReferenceType");
                if (rt && (strcmp(rt, "UsedBy") == 0 || strcmp(rt, "TypeInstance") == 0)) {
                    sb_printf(users, "%s%s", n ? ", " : "", tdi_s(r, "Name") ? tdi_s(r, "Name") : "?");
                    n++;
                    break;
                }
            }
            cJSON_Delete(locs);
        }
        cJSON_Delete(refs);
    }
    cJSON_Delete(srcs);
    td_clear_err();
    return n;
}

static int a_delete(tool_ctx *c)
{
    nav_plc plc;
    sw_found u;
    if (resolve_udt(c, &plc, &u) != 0)
        return -1;
    strbuf users;
    sb_init(&users);
    int n = referenced(c, u.item, &users);
    if (n > 0) {
        fail(c, "'%s' is still used by %d object(s): %s. Remove those uses first.", u.name, n, sb_str(&users));
        sb_free(&users);
        return -1;
    }
    sb_free(&users);
    if (td_call_v(u.item, "Delete", NULL) != 0)
        return fail_td(c, "delete failed");
    out(c, "PLC data type '%s' deleted.\n", u.name);
    return 0;
}

static int a_add_member(tool_ctx *c)
{
    nav_plc plc;
    sw_found u;
    if (resolve_udt(c, &plc, &u) != 0)
        return -1;
    const cJSON *bulk = arg_arr(c, "members");
    ed_doc d;
    if (bulk) {
        bg_member *members = NULL;
        int n = 0;
        if (bg_parse_interface(c, bulk, plc.software, SECTION, &members, &n) != 0)
            return -1;
        if (ed_open(c, session_project(), u.item, "Types", &d) != 0) {
            free(members);
            return -1;
        }
        for (int i = 0; i < n; i++) {
            if (ed_add_member(c, &d, SECTION, members[i].name, members[i].type, members[i].start, members[i].comment) != 0) {
                free(members);
                ed_close(&d);
                return -1;
            }
        }
        free(members);
        if (!ed_commit(c, &d))
            return -1;
        out(c, "%d member(s) added to %s.\n", n, u.name);
        return 0;
    }
    const char *name = arg_req(c, "memberName");
    const char *type = name ? arg_req(c, "dataType") : NULL;
    if (!type)
        return -1;
    char dt[512];
    mb_normalize_datatype(plc.software, type, dt, sizeof dt);
    if (ed_open(c, session_project(), u.item, "Types", &d) != 0)
        return -1;
    const char *start = arg_s(c, "initialValue");
    if (ed_add_member(c, &d, SECTION, name, dt, start ? start : arg_s(c, "startValue"), arg_s(c, "comment")) != 0) {
        ed_close(&d);
        return -1;
    }
    if (!ed_commit(c, &d))
        return -1;
    out(c, "Member '%s : %s' added to %s.\n", name, dt, u.name);
    return 0;
}

static int a_delete_member(tool_ctx *c)
{
    nav_plc plc;
    sw_found u;
    if (resolve_udt(c, &plc, &u) != 0)
        return -1;
    const char *name = arg_req(c, "memberName");
    if (!name)
        return -1;
    ed_doc d;
    if (ed_open(c, session_project(), u.item, "Types", &d) != 0)
        return -1;
    mxml_node_t *m = ed_member(c, &d, NULL, name, NULL);
    if (!m) {
        ed_close(&d);
        return -1;
    }
    mxml_node_t *sec = mb_section(d.sections, SECTION);
    int count = 0;
    for (mxml_node_t *x = sml_child(sec, "Member"); x; x = sml_next(x, "Member"))
        count++;
    if (!strchr(name, '.') && count <= 1) {
        ed_close(&d);
        return fail(c, "'%s' is the last member: TIA rejects an empty STRUCT", name);
    }
    mxmlDelete(m);
    if (!ed_commit(c, &d))
        return -1;
    out(c, "Member '%s' deleted from %s.\n", name, u.name);
    return 0;
}

static int a_update_member(tool_ctx *c)
{
    nav_plc plc;
    sw_found u;
    if (resolve_udt(c, &plc, &u) != 0)
        return -1;
    const char *name = arg_req(c, "memberName");
    if (!name)
        return -1;
    const char *nt = arg_s(c, "newDataType");
    const char *ns = arg_s(c, "newInitialValue");
    if (!ns)
        ns = arg_s(c, "newStartValue");
    const char *nc = arg_s(c, "newComment");
    const char *nn = arg_s(c, "newName");
    if (!nt && !ns && !nc && !nn)
        return fail(c, "pass at least one of newDataType, newInitialValue, newComment, newName");
    char dt[512] = "";
    if (nt)
        mb_normalize_datatype(plc.software, nt, dt, sizeof dt);
    ed_doc d;
    if (ed_open(c, session_project(), u.item, "Types", &d) != 0)
        return -1;
    mxml_node_t *m = ed_member(c, &d, NULL, name, NULL);
    if (!m) {
        ed_close(&d);
        return -1;
    }
    ed_update_member(c, &d, m, nn, nt ? dt : NULL, ns, nc);
    if (!ed_commit(c, &d))
        return -1;
    out(c, "Member '%s' of %s updated.\n", name, u.name);
    return 0;
}

static int a_update_member_comment(tool_ctx *c)
{
    if (!arg_has(c, "newComment"))
        return fail(c, "missing required argument 'newComment' (not 'comment')");
    return a_update_member(c);
}

static int a_move(tool_ctx *c)
{
    nav_plc plc;
    sw_found u;
    if (resolve_udt(c, &plc, &u) != 0)
        return -1;
    if (!arg_has(c, "targetFolder"))
        return fail(c, "missing required argument 'targetFolder' ('' = container root)");
    th target = sw_folder(c, plc.software, SWC_TYPES, arg_s(c, "targetFolder"), 1, arg_b(c, "createParents", 0));
    if (!target)
        return -1;
    th res = mb_move(c, u.item, "Types", target);
    if (!res)
        return -1;
    out(c, "PLC data type '%s' moved to '%s'.\n", u.name, *arg_s(c, "targetFolder") ? arg_s(c, "targetFolder") : "(root)");
    return 0;
}

static const action_def actions[] = {
    { "add_member", "deviceName, udtName, memberName, dataType; optional comment, initialValue | members[]",
      "Add a member (or several with members[] = [{name, dataType, startValue, comment}]) via XML round-trip. Dotted "
      "names add inside a Struct member.",
      a_add_member, AF_PROJECT | AF_WRITES },
    { "create", "deviceName, udtName; optional members[], targetFolder, createParents",
      "New PLC data type. Without members[] it gets a placeholder member _placeholder (Bool) because TIA rejects an empty "
      "STRUCT - remove it with delete_member later.",
      a_create, AF_PROJECT | AF_WRITES },
    { "delete", "deviceName, udtName", "Delete a PLC data type. Refused while it is still referenced.", a_delete,
      AF_PROJECT | AF_WRITES | AF_DESTRUCTIVE },
    { "delete_member", "deviceName, udtName, memberName", "Remove a member (refuses the last one).", a_delete_member,
      AF_PROJECT | AF_WRITES },
    { "export_xml", "deviceName, udtName; optional outputPath, returnInline=true, outputDirectory",
      "Export to SimaticML. With outputPath it goes to disk, otherwise inline (large content goes to the export store).",
      a_export_xml, AF_PROJECT },
    { "get_structure", "deviceName, udtName", "Member list with types, start values, access flags and comments.",
      a_get_structure, AF_PROJECT },
    { "import_xml", "deviceName, filePath OR xmlContent; optional overwrite=false, targetFolder, createParents",
      "Import a PLC data type from SimaticML. overwrite=true replaces the whole object (not a merge). Reliable for "
      "round-trips; for synthesized types prefer create + add_member.",
      a_import_xml, AF_PROJECT | AF_WRITES },
    { "list", "deviceName", "All PLC data types with folder-qualified names, consistency and modification date.", a_list,
      AF_PROJECT },
    { "move", "deviceName, udtName, targetFolder; optional createParents=false",
      "OFFLINE REQUIRED. Move to another folder ('' = root): export -> delete -> import, restored on failure.", a_move,
      AF_PROJECT | AF_WRITES | AF_OFFLINE },
    { "update_member", "deviceName, udtName, memberName; optional newDataType, newInitialValue, newComment, newName",
      "Update member properties (at least one). Empty values remove start value/comment.", a_update_member,
      AF_PROJECT | AF_WRITES },
    { "update_member_comment", "deviceName, udtName, memberName, newComment",
      "Comment-only update. The argument is 'newComment', NOT 'comment'.", a_update_member_comment, AF_PROJECT | AF_WRITES },
};

const tool_def tool_udt = {
    .name = "udt",
    .title = "PLC data types",
    .summary = "User-defined PLC data types (UDT): list, structure, create, members, import/export XML, move, delete.",
    .properties =
        "{"
        "\"deviceName\":{\"type\":\"string\",\"description\":\"Device or PLC name (session list_devices).\"},"
        "\"udtName\":{\"type\":\"string\",\"description\":\"PLC data type name (bare or folder-qualified).\"},"
        "\"memberName\":{\"type\":\"string\"},\"dataType\":{\"type\":\"string\"},"
        "\"initialValue\":{\"type\":\"string\"},\"comment\":{\"type\":\"string\"},"
        "\"members\":{\"type\":\"array\",\"items\":{\"type\":\"object\"},\"description\":\"[{name, dataType, startValue, comment}]\"},"
        "\"newDataType\":{\"type\":\"string\"},\"newInitialValue\":{\"type\":\"string\"},"
        "\"newComment\":{\"type\":\"string\"},\"newName\":{\"type\":\"string\"},"
        "\"filePath\":{\"type\":\"string\"},\"xmlContent\":{\"type\":\"string\"},\"overwrite\":{\"type\":\"boolean\"},"
        "\"outputPath\":{\"type\":\"string\"},\"outputDirectory\":{\"type\":\"string\"},\"returnInline\":{\"type\":\"boolean\"},"
        "\"targetFolder\":{\"type\":\"string\"},\"createParents\":{\"type\":\"boolean\"}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
};
