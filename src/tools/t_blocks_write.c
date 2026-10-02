/* blocks_write: create, import, copy/move/rename/delete blocks, interface and network editing. */
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

/* ---- shared ------------------------------------------------------------------------- */

static int block_exists(th plc_software, const char *name, sw_found *f)
{
    tool_ctx probe;
    ctx_init(&probe, NULL, NULL);
    int found = sw_find(&probe, plc_software, SWC_BLOCKS, name, f) == 0;
    ctx_free(&probe);
    td_clear_err();
    return found;
}

static th target_group(tool_ctx *c, nav_plc *plc)
{
    return sw_folder(c, plc->software, SWC_BLOCKS, arg_s(c, "targetFolder"), 1, arg_b(c, "createParents", 0));
}

/* Reports the new block and compiles it when autoCompile (default true). */
static int finish_block(tool_ctx *c, nav_plc *plc, const char *name, const char *verb)
{
    sw_found b;
    if (!block_exists(plc->software, name, &b))
        return fail(c, "block '%s' not found after %s", name, verb);
    long long num = 0;
    td_get_i(b.item, "Number", &num);
    char *lang = td_get_s(b.item, "ProgrammingLanguage");
    out(c, "%s %s%s%s [%s%lld, %s] %s.\n", b.type, b.folder, *b.folder ? "/" : "", b.name,
        strstr(b.type, "DB") ? "DB" : b.type, num, lang ? lang : "?", verb);
    free(lang);
    if (arg_b(c, "autoCompile", 1))
        compile_object(c, b.item, 1);
    return 0;
}

static int check_language(tool_ctx *c, const char *lang)
{
    if (!lang)
        return 0;
    if (_stricmp(lang, "STL") == 0)
        return fail(c, "language STL is not supported for new blocks (Openness limitation): use SCL, LAD or FBD");
    if (_stricmp(lang, "SCL") && _stricmp(lang, "LAD") && _stricmp(lang, "FBD"))
        return fail(c, "language must be LAD, FBD or SCL");
    return 0;
}

static const char *upper_lang(const char *lang, const char *def)
{
    if (!lang || !*lang)
        return def;
    if (_stricmp(lang, "SCL") == 0)
        return "SCL";
    if (_stricmp(lang, "FBD") == 0)
        return "FBD";
    return "LAD";
}

typedef struct ob_numbers {
    char used[65536];
} ob_numbers;

static int ob_num_cb(void *ctx, const cJSON *item, const char *folder)
{
    (void)folder;
    ob_numbers *o = ctx;
    long long n = tdi_i(item, "Number", 0);
    if (strcmp(short_type(item), "OB") == 0 && n > 0 && n < 65536)
        o->used[n] = 1;
    return 0;
}

/* OBs need an explicit number: first free one from 123. */
static long long next_ob_number(th plc_software)
{
    ob_numbers *o = calloc(1, sizeof *o);
    if (!o)
        return 123;
    sw_walk(plc_software, SWC_BLOCKS, "Number", ob_num_cb, NULL, o);
    long long n = 123;
    while (n < 65535 && o->used[n])
        n++;
    free(o);
    return n;
}

static int create_from_spec(tool_ctx *c, nav_plc *plc, bg_spec *s, const char *scl_code, int overwrite)
{
    if (strcmp(s->kind, "OB") == 0 && s->number <= 0)
        s->number = next_ob_number(plc->software);
    sw_found ex;
    int exists = block_exists(plc->software, s->name, &ex);
    if (exists && !overwrite)
        return fail(c, "a block named '%s' already exists (%s in '%s'). Pass overwrite=true to replace it.", s->name, ex.type,
                    *ex.folder ? ex.folder : "(root)");
    th group;
    if (exists && !arg_has(c, "targetFolder"))
        group = td_get_h(ex.item, "Parent"); /* replace in place */
    else
        group = target_group(c, plc);
    if (!group)
        return -1;
    s->culture = mb_editing_language(session_project());
    if (scl_code && *scl_code) {
        char *src = bg_build_scl(s, scl_code);
        int user_group = td_is(group, "Siemens.Engineering.SW.Blocks.PlcBlockUserGroup");
        th res = mb_generate_from_source(c, plc->software, s->name, ".scl", src, user_group ? group : 0);
        free(src);
        if (!res)
            return -1;
        if (s->number > 0) {
            td_set(res, "AutoNumber", cJSON_CreateBool(0));
            if (td_set(res, "Number", cJSON_CreateNumber((double)s->number)) != 0)
                out(c, "Warning: number %lld could not be set (%s).\n", s->number, td_err());
        }
        if (s->comment && *s->comment)
            mb_ml_set(res, "Comment", s->culture, s->comment);
        td_clear_err();
    } else {
        mxml_node_t *top = bg_build_xml(s);
        th res = mb_import_tree(c, group, "Blocks", top, exists);
        mxmlDelete(top);
        if (!res)
            return -1;
    }
    return finish_block(c, plc, s->name, exists ? "replaced" : "created");
}

/* ---- create ------------------------------------------------------------------------------ */

static int create_simple(tool_ctx *c, const char *kind, const char *name_arg, const char *number_arg)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *name = arg_req(c, name_arg);
    if (!name)
        return -1;
    const char *lang = arg_s(c, "language");
    if (check_language(c, lang) != 0)
        return -1;
    bg_spec s;
    memset(&s, 0, sizeof s);
    s.kind = kind;
    s.name = name;
    s.number = arg_i(c, number_arg, 0);
    s.language = upper_lang(lang, "LAD");
    s.author = arg_s(c, "author");
    s.version = arg_s(c, "version");
    s.return_type = arg_s(c, "returnType");
    if (strcmp(kind, "OB") == 0 && s.number >= 2 && s.number <= 122)
        return fail(c, "OB numbers 2-122 are reserved for event OBs: omit obNumber (automatic, from 123) or use 1");
    return create_from_spec(c, &plc, &s, NULL, 0);
}

static int a_create_fb(tool_ctx *c) { return create_simple(c, "FB", "fbName", "fbNumber"); }
static int a_create_fc(tool_ctx *c) { return create_simple(c, "FC", "fcName", "fcNumber"); }
static int a_create_ob(tool_ctx *c) { return create_simple(c, "OB", "obName", "obNumber"); }

static int a_create_block(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *name = arg_req(c, "blockName");
    const char *type = name ? arg_req(c, "blockType") : NULL;
    if (!type)
        return -1;
    const char *kind = _stricmp(type, "FB") == 0 ? "FB" : _stricmp(type, "FC") == 0 ? "FC" : _stricmp(type, "OB") == 0 ? "OB" : NULL;
    if (!kind)
        return fail(c, "blockType must be FB, FC or OB (data blocks: db action=create)");
    const char *lang = arg_s(c, "language");
    if (check_language(c, lang) != 0)
        return -1;
    const char *scl = arg_s(c, "sclCode");
    if (scl && *scl && lang && _stricmp(lang, "SCL") != 0)
        return fail(c, "sclCode requires language=SCL");
    bg_member *members = NULL;
    int n = 0;
    if (bg_parse_interface(c, arg_get(c, "interface"), plc.software, "Static", &members, &n) != 0)
        return -1;
    const cJSON *nets = arg_arr(c, "networks");
    const char *titles[64];
    int nnet = 0;
    const cJSON *net;
    cJSON_ArrayForEach(net, nets)
    {
        if (cJSON_GetObjectItem(net, "rungs")) {
            free(members);
            return fail(c, "networks[].rungs (LAD rung builder) is not available in this version: create the block, then "
                           "import SimaticML with import_xml_inline, or use SCL with sclCode");
        }
        if (nnet < 64)
            titles[nnet++] = cJSON_GetStringValue(cJSON_GetObjectItem(net, "title"));
    }
    bg_spec s;
    memset(&s, 0, sizeof s);
    s.kind = kind;
    s.name = name;
    s.number = arg_i(c, "blockNumber", 0);
    s.language = upper_lang(lang, scl && *scl ? "SCL" : "LAD");
    s.author = arg_s(c, "author");
    s.version = arg_s(c, "version");
    s.title = arg_s(c, "blockTitle");
    s.comment = arg_s(c, "blockComment");
    s.memory_layout = arg_s(c, "memoryLayout");
    s.secondary_type = arg_s(c, "secondaryType");
    s.return_type = arg_s(c, "returnType");
    s.members = members;
    s.nmembers = n;
    s.networks = nnet;
    s.network_titles = nnet ? titles : NULL;
    int rc = create_from_spec(c, &plc, &s, scl, arg_b(c, "overwrite", 0));
    free(members);
    return rc;
}

/* ---- import ----------------------------------------------------------------------------- */

static int import_common(tool_ctx *c, const char *path)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    char err[256];
    mxml_node_t *top = sml_load_file(path, err, sizeof err);
    if (!top)
        return fail(c, "invalid XML: %s", err);
    mxml_node_t *obj = sml_object(top);
    const char *kind = obj ? mxmlGetElement(obj) : NULL;
    char name[256] = "";
    const char *n = obj ? sml_child_text(sml_attribute_list(obj), "Name") : NULL;
    snprintf(name, sizeof name, "%s", n ? n : "");
    int is_block = kind && strncmp(kind, "SW.Blocks.", 10) == 0;
    mxmlDelete(top);
    if (!is_block)
        return fail(c, "the document does not contain a block (found %s); PLC data types: udt action=import_xml, tag tables: "
                       "tag action=import_table",
                    kind ? kind : "nothing");
    int overwrite = arg_b(c, "overwrite", 0);
    sw_found ex;
    memset(&ex, 0, sizeof ex);
    int exists = *name && block_exists(plc.software, name, &ex);
    if (exists && !overwrite)
        return fail(c, "block '%s' already exists: pass overwrite=true to replace it. OVERWRITE IS A WHOLE-OBJECT REPLACE, "
                       "NOT A MERGE.",
                    name);
    th group = exists && !arg_has(c, "targetFolder") ? td_get_h(ex.item, "Parent") : target_group(c, &plc);
    if (!group)
        return -1;
    if (!mb_import_file(c, group, "Blocks", path, overwrite))
        return -1;
    return finish_block(c, &plc, name, exists ? "replaced" : "imported");
}

static int a_import_xml_file(tool_ctx *c)
{
    const char *file = arg_req(c, "filePath");
    if (!file)
        return -1;
    char path[1024];
    if (fs_full_path(file, path, sizeof path) != 0 || !fs_is_file(path))
        return fail(c, "file '%s' not found", file);
    return import_common(c, path);
}

static int a_import_xml_inline(tool_ctx *c)
{
    const char *xml = arg_req(c, "xmlContent");
    if (!xml)
        return -1;
    char path[1024];
    if (fs_temp_path("inline", ".xml", path, sizeof path) != 0 || fs_write_all(path, xml, strlen(xml)) != 0)
        return fail(c, "cannot write a temporary file");
    int rc = import_common(c, path);
    fs_remove(path);
    return rc;
}

/* ---- manage --------------------------------------------------------------------------------- */

static int resolve(tool_ctx *c, nav_plc *plc, sw_found *b, const char *arg)
{
    if (sw_plc(c, plc) != 0)
        return -1;
    const char *name = arg_req(c, arg);
    if (!name)
        return -1;
    snprintf(c->error_context, sizeof c->error_context, "device=%s block=%s", plc->device_name, name);
    return sw_find(c, plc->software, SWC_BLOCKS, name, b);
}

static int a_delete_block(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve(c, &plc, &b, "blockName") != 0)
        return -1;
    if (strcmp(b.type, "InstanceDB") == 0 && !arg_b(c, "force", 0))
        return fail(c, "'%s' is an instance DB: pass force=true to delete it (e.g. an orphaned instance DB)", b.name);
    if (td_call_v(b.item, "Delete", NULL) != 0)
        return fail_td(c, "delete failed");
    out(c, "%s '%s' deleted.\n", b.type, b.name);
    return 0;
}

static int a_rename_block(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve(c, &plc, &b, "blockName") != 0)
        return -1;
    const char *nn = arg_s(c, "newName");
    long long num = arg_i(c, "newNumber", 0);
    if ((!nn || !*nn) && num <= 0)
        return fail(c, "pass newName and/or newNumber");
    if (nn && *nn) {
        sw_found ex;
        if (block_exists(plc.software, nn, &ex))
            return fail(c, "a block named '%s' already exists", nn);
        if (td_set(b.item, "Name", cJSON_CreateString(nn)) != 0)
            return fail_td(c, "rename failed");
    }
    if (num > 0) {
        td_set(b.item, "AutoNumber", cJSON_CreateBool(0));
        td_clear_err();
        if (td_set(b.item, "Number", cJSON_CreateNumber((double)num)) != 0)
            return fail_td(c, "renumbering failed");
    }
    out(c, "Block '%s' updated%s%s%s. Callers are NOT updated automatically: compile and fix them (xref find_callers).\n",
        b.name, nn && *nn ? ": new name " : "", nn && *nn ? nn : "", num > 0 ? " (new number set)" : "");
    return 0;
}

static int a_copy_block(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve(c, &plc, &b, "blockName") != 0)
        return -1;
    const char *nn = arg_req(c, "newName");
    if (!nn)
        return -1;
    sw_found ex;
    if (block_exists(plc.software, nn, &ex))
        return fail(c, "a block named '%s' already exists (block names are unique in the whole PLC)", nn);
    th group = arg_has(c, "targetFolder") ? target_group(c, &plc) : td_get_h(b.item, "Parent");
    if (!group)
        return -1;
    mxml_node_t *top = sw_export_tree(c, b.item);
    if (!top)
        return -1;
    mxml_node_t *al = sml_attribute_list(sml_object(top));
    sml_set_text(sml_child(al, "Name"), nn);
    mxml_node_t *num = sml_child(al, "Number");
    if (num)
        mxmlDelete(num);
    mxml_node_t *autonum = sml_child(al, "AutoNumber");
    if (autonum)
        sml_set_text(autonum, "true");
    th res = mb_import_tree(c, group, "Blocks", top, 0);
    mxmlDelete(top);
    if (!res)
        return -1;
    out(c, "Copied '%s' to '%s' (next free number).\n", b.name, nn);
    return finish_block(c, &plc, nn, "created");
}

static int a_move_block(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve(c, &plc, &b, "blockName") != 0)
        return -1;
    if (!arg_has(c, "targetFolder"))
        return fail(c, "missing required argument 'targetFolder' ('' or '/' = root)");
    th target = target_group(c, &plc);
    if (!target || !mb_move(c, b.item, "Blocks", target))
        return -1;
    const char *tf = arg_s(c, "targetFolder");
    out(c, "Block '%s' moved to '%s'.\n", b.name, tf && *tf && strcmp(tf, "/") ? tf : "(root)");
    return finish_block(c, &plc, b.name, "verified");
}

/* ---- interface editing ----------------------------------------------------------------------- */

static int is_call_section(const char *s)
{
    return s && (_stricmp(s, "Input") == 0 || _stricmp(s, "Output") == 0 || _stricmp(s, "InOut") == 0 || _stricmp(s, "Return") == 0);
}

typedef struct idb_ctx {
    const char *fb;
    strbuf *sb;
    int n;
} idb_ctx;

static int idb_cb(void *ctx, const cJSON *item, const char *folder)
{
    idb_ctx *k = ctx;
    if (strcmp(short_type(item), "InstanceDB") != 0)
        return 0;
    char *of = td_get_s(tdv_h(item), "InstanceOfName");
    if (of && _stricmp(of, k->fb) == 0) {
        sb_printf(k->sb, "  instance DB %s%s%s\n", folder, *folder ? "/" : "", tdi_s(item, "Name"));
        k->n++;
    }
    free(of);
    return 0;
}

/* Lists what an interface change affects; returns the number of impacted objects. */
static int blast_radius(tool_ctx *c, nav_plc *plc, sw_found *b, const char *section, strbuf *report)
{
    (void)c;
    int n = 0;
    if (is_call_section(section)) {
        th svc = td_service(b->item, "Siemens.Engineering.CrossReference.CrossReferenceService");
        th res = svc ? td_call_h(svc, "GetCrossReferences", tda("e", "Siemens.Engineering.CrossReference.CrossReferenceFilter", "AllObjects")) : 0;
        cJSON *srcs = res ? td_enum(td_get_h(res, "Sources"), NULL, -1) : NULL;
        const cJSON *s;
        cJSON_ArrayForEach(s, srcs)
        {
            cJSON *refs = td_enum(td_get_h(tdv_h(s), "References"), "Name,TypeName", -1);
            const cJSON *r;
            cJSON_ArrayForEach(r, refs)
            {
                cJSON *locs = td_enum(td_get_h(tdv_h(r), "Locations"), "ReferenceType,Access", -1);
                const cJSON *l;
                cJSON_ArrayForEach(l, locs)
                {
                    const char *rt = tdi_s(l, "ReferenceType");
                    if (rt && strcmp(rt, "UsedBy") == 0) {
                        sb_printf(report, "  call site in %s (%s)\n", tdi_s(r, "Name") ? tdi_s(r, "Name") : "?",
                                  tdi_s(r, "TypeName") ? tdi_s(r, "TypeName") : "?");
                        n++;
                        break;
                    }
                }
                cJSON_Delete(locs);
            }
            cJSON_Delete(refs);
        }
        cJSON_Delete(srcs);
    }
    if (strcmp(b->type, "FB") == 0 && section && _stricmp(section, "Temp") != 0 && _stricmp(section, "Constant") != 0) {
        idb_ctx k = { b->name, report, 0 };
        sw_walk(plc->software, SWC_BLOCKS, "Name", idb_cb, NULL, &k);
        if (k.n)
            sb_append(report, "  (downloading changed instance DBs re-initialises their actual values: see "
                              "reinitializeDataBlocks)\n");
        n += k.n;
    }
    td_clear_err();
    return n;
}

static int gate(tool_ctx *c, nav_plc *plc, sw_found *b, const char *section, const char *what)
{
    strbuf report;
    sb_init(&report);
    int n = blast_radius(c, plc, b, section, &report);
    int rc = 0;
    if (n > 0 && !confirmed(c, "I understand")) {
        out(c, "BLAST RADIUS of %s on %s:\n%s", what, b->name, sb_str(&report));
        rc = fail(c, "%d dependent object(s) will need recompilation/update. Repeat with confirm='I understand' to proceed.", n);
    } else if (n > 0) {
        out(c, "Affected (confirmed):\n%s", sb_str(&report));
    }
    sb_free(&report);
    return rc;
}

static int interface_edit_finish(tool_ctx *c, nav_plc *plc, ed_doc *d, const char *name, const char *msg)
{
    if (!ed_commit(c, d))
        return -1;
    out(c, "%s\n", msg);
    return finish_block(c, plc, name, "updated");
}

static int a_add_interface_member(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve(c, &plc, &b, "blockName") != 0)
        return -1;
    const char *section = arg_req(c, "section");
    const char *name = section ? arg_req(c, "memberName") : NULL;
    const char *type = name ? arg_req(c, "dataType") : NULL;
    if (!type)
        return -1;
    if (strstr(b.type, "DB"))
        return fail(c, "'%s' is a data block: use db action=add_member", b.name);
    if (gate(c, &plc, &b, section, "adding a member") != 0)
        return -1;
    char dt[512];
    mb_normalize_datatype(plc.software, type, dt, sizeof dt);
    ed_doc d;
    if (ed_open(c, session_project(), b.item, "Blocks", &d) != 0)
        return -1;
    mxml_node_t *sec = mb_section(d.sections, section);
    if (!sec) {
        ed_close(&d);
        return fail(c, "section '%s' does not exist in a %s", section, b.type);
    }
    if (ed_add_member(c, &d, sml_attr(sec, "Name"), name, dt, arg_s(c, "startValue"), arg_s(c, "comment")) != 0) {
        ed_close(&d);
        return -1;
    }
    char msg[700];
    snprintf(msg, sizeof msg, "Member '%s : %s' added to %s.%s.", name, dt, b.name, sml_attr(sec, "Name"));
    return interface_edit_finish(c, &plc, &d, b.name, msg);
}

static int a_update_interface_member(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve(c, &plc, &b, "blockName") != 0)
        return -1;
    const char *name = arg_req(c, "memberName");
    if (!name)
        return -1;
    const char *nn = arg_s(c, "newName"), *nt = arg_s(c, "newDataType"), *ns = arg_s(c, "newStartValue"),
               *nc = arg_s(c, "newComment");
    if (!nn && !nt && !ns && !nc)
        return fail(c, "pass at least one of newName, newDataType, newStartValue, newComment");
    ed_doc d;
    if (ed_open(c, session_project(), b.item, "Blocks", &d) != 0)
        return -1;
    const char *section = NULL;
    mxml_node_t *m = ed_member(c, &d, arg_s(c, "section"), name, &section);
    if (!m) {
        ed_close(&d);
        return -1;
    }
    if ((nn || nt) && gate(c, &plc, &b, section, "changing a member") != 0) {
        ed_close(&d);
        return -1;
    }
    char dt[512] = "";
    if (nt)
        mb_normalize_datatype(plc.software, nt, dt, sizeof dt);
    ed_update_member(c, &d, m, nn, nt ? dt : NULL, ns, nc);
    char msg[600];
    snprintf(msg, sizeof msg, "Member '%s' (%s) of %s updated.", name, section ? section : "?", b.name);
    return interface_edit_finish(c, &plc, &d, b.name, msg);
}

static int a_delete_interface_member(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    if (resolve(c, &plc, &b, "blockName") != 0)
        return -1;
    const char *name = arg_req(c, "memberName");
    if (!name)
        return -1;
    ed_doc d;
    if (ed_open(c, session_project(), b.item, "Blocks", &d) != 0)
        return -1;
    const char *section = NULL;
    mxml_node_t *m = ed_member(c, &d, arg_s(c, "section"), name, &section);
    if (!m) {
        ed_close(&d);
        return -1;
    }
    if (gate(c, &plc, &b, section, "deleting a member") != 0) {
        ed_close(&d);
        return -1;
    }
    mxmlDelete(m);
    char msg[600];
    snprintf(msg, sizeof msg, "Member '%s' (%s) deleted from %s. Logic still using it fails to compile (xref get_references).",
             name, section ? section : "?", b.name);
    return interface_edit_finish(c, &plc, &d, b.name, msg);
}

static int a_add_multi_instance_member(tool_ctx *c)
{
    nav_plc plc;
    sw_found fb;
    if (resolve(c, &plc, &fb, "parentFB") != 0)
        return -1;
    if (strcmp(fb.type, "FB") != 0)
        return fail(c, "'%s' is a %s: multi-instances live in FBs", fb.name, fb.type);
    const char *name = arg_req(c, "memberName");
    const char *inner = name ? arg_req(c, "innerType") : NULL;
    if (!inner)
        return -1;
    char dt[512];
    mb_normalize_datatype(plc.software, inner, dt, sizeof dt);
    if (dt[0] != '"')
        out(c, "Note: '%s' is not an FB of this PLC; using it as a system type (e.g. TON_TIME).\n", inner);
    ed_doc d;
    if (ed_open(c, session_project(), fb.item, "Blocks", &d) != 0)
        return -1;
    const char *existing_section = NULL;
    mxml_node_t *ex = sml_find_member(d.sections, "Static", name, &existing_section);
    if (ex) {
        const char *on = arg_s(c, "onCollision");
        if (!on || _stricmp(on, "replace") != 0) {
            ed_close(&d);
            return fail(c, "static member '%s' already exists: pass onCollision=replace to replace it", name);
        }
        mxmlDelete(ex);
    }
    if (ed_add_member(c, &d, "Static", name, dt, NULL, arg_s(c, "comment")) != 0) {
        ed_close(&d);
        return -1;
    }
    char msg[700];
    snprintf(msg, sizeof msg, "Multi-instance '%s : %s' added to %s.", name, dt, fb.name);
    return interface_edit_finish(c, &plc, &d, fb.name, msg);
}

static int a_remove_multi_instance_member(tool_ctx *c)
{
    nav_plc plc;
    sw_found fb;
    if (resolve(c, &plc, &fb, "parentFB") != 0)
        return -1;
    const char *name = arg_req(c, "memberName");
    if (!name)
        return -1;
    ed_doc d;
    if (ed_open(c, session_project(), fb.item, "Blocks", &d) != 0)
        return -1;
    mxml_node_t *m = ed_member(c, &d, "Static", name, NULL);
    if (!m) {
        ed_close(&d);
        return -1;
    }
    mxmlDelete(m);
    char msg[600];
    snprintf(msg, sizeof msg, "Multi-instance '%s' removed from %s.", name, fb.name);
    return interface_edit_finish(c, &plc, &d, fb.name, msg);
}

/* ---- networks ---------------------------------------------------------------------------- */

static int max_id(mxml_node_t *top)
{
    int max = 0;
    for (mxml_node_t *n = top; n; n = mxmlWalkNext(n, top, MXML_DESCEND_ALL)) {
        if (mxmlGetType(n) != MXML_TYPE_ELEMENT)
            continue;
        const char *id = mxmlElementGetAttr(n, "ID");
        if (id) {
            int v = (int)strtol(id, NULL, 16);
            if (v > max)
                max = v;
        }
    }
    return max;
}

static void set_ml(mxml_node_t *cu, const char *composition, const char *culture, const char *text, int *next_id)
{
    mxml_node_t *ol = sml_ensure_child(cu, "ObjectList", 0);
    mxml_node_t *mt;
    for (mt = sml_child(ol, "MultilingualText"); mt; mt = sml_next(mt, "MultilingualText"))
        if (sml_attr(mt, "CompositionName") && strcmp(sml_attr(mt, "CompositionName"), composition) == 0)
            break;
    if (!mt) {
        mt = mxmlNewElement(ol, "MultilingualText");
        mxmlElementSetAttrf(mt, "ID", "%X", (*next_id)++);
        mxmlElementSetAttr(mt, "CompositionName", composition);
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
    mxml_node_t *t = sml_ensure_child(sml_ensure_child(item, "AttributeList", 0), "Text", 0);
    sml_set_text(t, text);
}

static int network_block(tool_ctx *c, nav_plc *plc, sw_found *b, ed_doc *d, int lad_fbd_only)
{
    if (resolve(c, plc, b, "blockName") != 0)
        return -1;
    if (strstr(b->type, "DB"))
        return fail(c, "'%s' is a data block: it has no networks", b->name);
    if (ed_open(c, session_project(), b->item, "Blocks", d) != 0)
        return -1;
    if (lad_fbd_only) {
        const char *lang = sml_child_text(sml_attribute_list(d->obj), "ProgrammingLanguage");
        if (lang && (strcmp(lang, "SCL") == 0 || strcmp(lang, "STL") == 0)) {
            ed_close(d);
            return fail(c, "%s is an %s block with a single code section: networks can only be added/removed in LAD/FBD blocks",
                        b->name, lang);
        }
    }
    return 0;
}

static int a_add_network(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    ed_doc d;
    if (network_block(c, &plc, &b, &d, 1) != 0)
        return -1;
    int n = sml_compile_unit_count(d.obj);
    int pos = (int)arg_i(c, "position", n);
    if (pos < 0 || pos > n)
        pos = n;
    const char *lang = sml_child_text(sml_attribute_list(d.obj), "ProgrammingLanguage");
    int next = max_id(d.top) + 1;
    mxml_node_t *cu = mxmlNewElement(NULL, "SW.Blocks.CompileUnit");
    mxmlElementSetAttrf(cu, "ID", "%X", next++);
    mxmlElementSetAttr(cu, "CompositionName", "CompileUnits");
    mxml_node_t *al = mxmlNewElement(cu, "AttributeList");
    mxmlNewElement(al, "NetworkSource");
    mxmlNewOpaque(mxmlNewElement(al, "ProgrammingLanguage"), lang && strcmp(lang, "FBD") == 0 ? "FBD" : "LAD");
    set_ml(cu, "Comment", d.culture, arg_s(c, "comment"), &next);
    set_ml(cu, "Title", d.culture, arg_s(c, "title"), &next);
    mxml_node_t *ol = sml_child(d.obj, "ObjectList");
    mxml_node_t *anchor = pos < n ? sml_compile_unit(d.obj, pos) : (n ? sml_compile_unit(d.obj, n - 1) : NULL);
    if (pos < n)
        mxmlAdd(ol, MXML_ADD_BEFORE, anchor, cu);
    else if (anchor)
        mxmlAdd(ol, MXML_ADD_AFTER, anchor, cu);
    else
        mxmlAdd(ol, MXML_ADD_AFTER, NULL, cu);
    char msg[300];
    snprintf(msg, sizeof msg, "Empty network inserted at index %d of %s (now %d networks).", pos, b.name, n + 1);
    return interface_edit_finish(c, &plc, &d, b.name, msg);
}

static int a_delete_network(tool_ctx *c)
{
    nav_plc plc;
    sw_found b;
    ed_doc d;
    if (!arg_has(c, "networkIndex"))
        return fail(c, "missing required argument 'networkIndex' (0-based)");
    if (network_block(c, &plc, &b, &d, 1) != 0)
        return -1;
    int n = sml_compile_unit_count(d.obj);
    int idx = (int)arg_i(c, "networkIndex", -1);
    if (idx < 0 || idx >= n) {
        ed_close(&d);
        return fail(c, "networkIndex %d out of range (0..%d)", idx, n - 1);
    }
    if (n <= 1) {
        ed_close(&d);
        return fail(c, "refusing to delete the last network of %s", b.name);
    }
    mxmlDelete(sml_compile_unit(d.obj, idx));
    char msg[300];
    snprintf(msg, sizeof msg, "Network %d deleted from %s (%d left).", idx, b.name, n - 1);
    return interface_edit_finish(c, &plc, &d, b.name, msg);
}

static int update_network_common(tool_ctx *c, int lad_only)
{
    nav_plc plc;
    sw_found b;
    ed_doc d;
    if (!arg_has(c, "networkIndex"))
        return fail(c, "missing required argument 'networkIndex' (0-based)");
    if (!arg_has(c, "title") && !arg_has(c, "comment"))
        return fail(c, "pass title and/or comment");
    if (network_block(c, &plc, &b, &d, lad_only) != 0)
        return -1;
    int n = sml_compile_unit_count(d.obj);
    int idx = (int)arg_i(c, "networkIndex", -1);
    if (idx < 0 || idx >= n) {
        ed_close(&d);
        return fail(c, "networkIndex %d out of range (0..%d)", idx, n - 1);
    }
    mxml_node_t *cu = sml_compile_unit(d.obj, idx);
    int next = max_id(d.top) + 1;
    if (arg_has(c, "title"))
        set_ml(cu, "Title", d.culture, arg_s(c, "title"), &next);
    if (arg_has(c, "comment"))
        set_ml(cu, "Comment", d.culture, arg_s(c, "comment"), &next);
    char msg[300];
    snprintf(msg, sizeof msg, "Network %d of %s updated.", idx, b.name);
    return interface_edit_finish(c, &plc, &d, b.name, msg);
}

static int a_update_network(tool_ctx *c) { return update_network_common(c, 0); }
static int a_replace_network(tool_ctx *c) { return update_network_common(c, 1); }

/* ---- table --------------------------------------------------------------------------------- */

#define W (AF_PROJECT | AF_WRITES)

static const action_def actions[] = {
    { "add_interface_member", "deviceName, blockName, section, memberName, dataType; optional startValue, comment, confirm",
      "Add a member to an FB/FC/OB interface section (Input, Output, InOut, Static, Temp, Constant). The BLAST RADIUS "
      "(call sites that go out of date, instance DBs) is computed first; if anything depends on the block the call is "
      "refused until confirm='I understand'.",
      a_add_interface_member, W },
    { "add_multi_instance_member", "deviceName, parentFB, memberName, innerType; optional onCollision=fail|replace",
      "Add a static multi-instance member of an FB type to an FB.", a_add_multi_instance_member, W },
    { "add_network", "deviceName, blockName; optional position, title, comment",
      "Insert an empty network at position (default: end). LAD/FBD only (SCL blocks have a single code section).",
      a_add_network, W },
    { "copy_block", "deviceName, blockName, newName; optional targetFolder, createParents=false",
      "Duplicate a block under a new (PLC-wide unique) name, next to the source unless targetFolder is given. The copy "
      "gets the next free number; the source is not modified.",
      a_copy_block, W },
    { "create_block",
      "deviceName, blockName, blockType=FB|FC|OB, language; optional blockNumber, interface, sclCode, networks, blockTitle, "
      "blockComment, author, version, memoryLayout, secondaryType, returnType, overwrite, targetFolder, createParents, "
      "autoCompile=true",
      "Full block creation. interface = {Input:[{name, dataType, startValue, comment}], Output:[...], ...}. SCL blocks "
      "with sclCode are compiled from a generated external source (statement part only, without VAR sections). LAD/FBD "
      "blocks get empty networks (networks=[{title}]). blockNumber automatic if omitted. overwrite=true replaces an "
      "existing block entirely.",
      a_create_block, W },
    { "create_fb", "deviceName, fbName; optional fbNumber, language=LAD|SCL|FBD, author, version, autoCompile=true, targetFolder, createParents=false",
      "New empty FB (STL not supported: Openness limitation). targetFolder 'Motors/Drives'; missing folders are an "
      "ERROR unless createParents=true.",
      a_create_fb, W },
    { "create_fc", "deviceName, fcName; optional fcNumber, language, returnType=Void, author, version, autoCompile=true, targetFolder, createParents=false",
      "New empty FC.", a_create_fc, W },
    { "create_ob", "deviceName, obName; optional obNumber, language, author, version, autoCompile=true, targetFolder, createParents=false",
      "New program-cycle OB. Number automatic if omitted (from 123); obNumber=1 allowed, 2-122 reserved.", a_create_ob, W },
    { "delete_block", "deviceName, blockName; optional force=false",
      "Delete a block. Instance DBs are refused unless force=true (useful for orphaned instance DBs).", a_delete_block,
      W | AF_DESTRUCTIVE },
    { "delete_interface_member", "deviceName, blockName, memberName; optional section, confirm",
      "Remove an interface member (same blast-radius confirm gate). Logic still using it will fail to compile.",
      a_delete_interface_member, W },
    { "delete_network", "deviceName, blockName, networkIndex", "Remove a network by 0-based index (never the last one). LAD/FBD only.",
      a_delete_network, W },
    { "import_xml_file", "deviceName, filePath; optional overwrite, autoCompile=true, targetFolder, createParents=false",
      "Import a block from a SimaticML file (preferred for large payloads). OVERWRITE IS A WHOLE-OBJECT REPLACE, NOT A "
      "MERGE. Without targetFolder an overwrite targets the folder the existing block lives in.",
      a_import_xml_file, W },
    { "import_xml_inline", "deviceName, xmlContent; optional overwrite, autoCompile=true, targetFolder, createParents=false",
      "Import a block from a SimaticML string (same rules as import_xml_file).", a_import_xml_inline, W },
    { "move_block", "deviceName, blockName, targetFolder; optional createParents=false",
      "OFFLINE REQUIRED. Move a block to another folder ('' or '/' = root): export -> delete -> import, restored to the "
      "original folder if the import fails, then recompiled.",
      a_move_block, W | AF_OFFLINE },
    { "remove_multi_instance_member", "deviceName, parentFB, memberName", "Remove a static multi-instance member.",
      a_remove_multi_instance_member, W },
    { "rename_block", "deviceName, blockName; optional newName, newNumber",
      "Rename and/or renumber. Callers are NOT updated automatically.", a_rename_block, W },
    { "replace_network", "deviceName, blockName, networkIndex; optional title, comment",
      "Replace a network's title/comment. LAD/FBD only (rebuild SCL blocks with create_block overwrite=true).",
      a_replace_network, W },
    { "update_interface_member", "deviceName, blockName, memberName; optional section, newName, newDataType, newStartValue, newComment, confirm",
      "Update or rename an interface member (section omitted = searched in all sections). Renames and type changes use "
      "the blast-radius confirm gate. Empty newStartValue/newComment removes them.",
      a_update_interface_member, W },
    { "update_network", "deviceName, blockName, networkIndex; optional title, comment",
      "Update a network's title/comment (0-based index), any language.", a_update_network, W },
};

const tool_def tool_blocks_write = {
    .name = "blocks_write",
    .title = "Create & edit blocks",
    .summary = "Create (FB/FC/OB, SCL from code, LAD/FBD skeletons), import SimaticML, copy/move/rename/delete blocks, edit "
               "interfaces (with blast-radius check) and networks. Changes stay in the project until session save. The LAD "
               "rung editor is not available in this version.",
    .properties =
        "{"
        "\"deviceName\":{\"type\":\"string\",\"description\":\"Device or PLC name (session list_devices).\"},"
        "\"blockName\":{\"type\":\"string\",\"description\":\"Block name (bare or folder-qualified).\"},"
        "\"fbName\":{\"type\":\"string\"},\"fcName\":{\"type\":\"string\"},\"obName\":{\"type\":\"string\"},"
        "\"fbNumber\":{\"type\":\"integer\"},\"fcNumber\":{\"type\":\"integer\"},\"obNumber\":{\"type\":\"integer\"},"
        "\"blockNumber\":{\"type\":\"integer\"},"
        "\"blockType\":{\"type\":\"string\",\"enum\":[\"FB\",\"FC\",\"OB\"]},"
        "\"language\":{\"type\":\"string\",\"enum\":[\"LAD\",\"FBD\",\"SCL\"]},"
        "\"interface\":{\"type\":\"object\",\"description\":\"{Input:[{name, dataType, startValue, comment}], Output:[], InOut:[], Static:[], Temp:[], Constant:[]}\"},"
        "\"sclCode\":{\"type\":\"string\",\"description\":\"SCL statements (body only, no VAR sections).\"},"
        "\"networks\":{\"type\":\"array\",\"items\":{\"type\":\"object\"},\"description\":\"LAD/FBD: [{title}] empty networks.\"},"
        "\"blockTitle\":{\"type\":\"string\"},\"blockComment\":{\"type\":\"string\"},"
        "\"author\":{\"type\":\"string\"},\"version\":{\"type\":\"string\"},"
        "\"memoryLayout\":{\"type\":\"string\",\"enum\":[\"Optimized\",\"Standard\"]},"
        "\"secondaryType\":{\"type\":\"string\",\"description\":\"OB event class, default ProgramCycle.\"},"
        "\"returnType\":{\"type\":\"string\",\"description\":\"FC return type, default Void.\"},"
        "\"overwrite\":{\"type\":\"boolean\"},\"autoCompile\":{\"type\":\"boolean\"},"
        "\"targetFolder\":{\"type\":\"string\"},\"createParents\":{\"type\":\"boolean\"},"
        "\"filePath\":{\"type\":\"string\"},\"xmlContent\":{\"type\":\"string\"},"
        "\"newName\":{\"type\":\"string\"},\"newNumber\":{\"type\":\"integer\"},\"force\":{\"type\":\"boolean\"},"
        "\"section\":{\"type\":\"string\",\"enum\":[\"Input\",\"Output\",\"InOut\",\"Static\",\"Temp\",\"Constant\"]},"
        "\"memberName\":{\"type\":\"string\"},\"dataType\":{\"type\":\"string\"},"
        "\"startValue\":{\"type\":\"string\"},\"comment\":{\"type\":\"string\"},"
        "\"newDataType\":{\"type\":\"string\"},\"newStartValue\":{\"type\":\"string\"},\"newComment\":{\"type\":\"string\"},"
        "\"confirm\":{\"type\":\"string\",\"description\":\"Blast-radius gate: must contain 'I understand'.\"},"
        "\"parentFB\":{\"type\":\"string\"},\"innerType\":{\"type\":\"string\"},"
        "\"onCollision\":{\"type\":\"string\",\"enum\":[\"fail\",\"replace\"]},"
        "\"networkIndex\":{\"type\":\"integer\"},\"position\":{\"type\":\"integer\"},\"title\":{\"type\":\"string\"},"
        "\"limit\":{\"type\":\"integer\"}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
};
