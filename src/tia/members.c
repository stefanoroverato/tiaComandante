#include "members.h"

#include "tia/simaticml.h"
#include "tia/tia_sw.h"
#include "util/fs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *mb_editing_language(th project)
{
    static char lang[32];
    lang[0] = 0;
    th ls = project ? td_get_h(project, "LanguageSettings") : 0;
    th el = ls ? td_get_h(ls, "EditingLanguage") : 0;
    char *culture = el ? td_get_s(el, "Culture") : NULL;
    snprintf(lang, sizeof lang, "%s", culture && *culture ? culture : "en-US");
    free(culture);
    td_clear_err();
    return lang;
}

static int is_named_type(th plc_software, const char *name)
{
    tool_ctx probe;
    ctx_init(&probe, NULL, NULL);
    sw_found f;
    int found = sw_find(&probe, plc_software, SWC_TYPES, name, &f) == 0;
    if (!found)
        found = sw_find(&probe, plc_software, SWC_BLOCKS, name, &f) == 0 && strcmp(f.type, "FB") == 0;
    ctx_free(&probe);
    td_clear_err();
    return found;
}

void mb_normalize_datatype(th plc_software, const char *in, char *out, size_t cap)
{
    while (isspace((unsigned char)*in))
        in++;
    /* Array[lo..hi, ...] of <type> */
    if (_strnicmp(in, "Array", 5) == 0) {
        const char *of = NULL;
        for (const char *p = in; *p; p++)
            if (_strnicmp(p, " of ", 4) == 0) {
                of = p;
                break;
            }
        if (of) {
            char inner[512];
            mb_normalize_datatype(plc_software, of + 4, inner, sizeof inner);
            snprintf(out, cap, "%.*s of %s", (int)(of - in), in, inner);
            return;
        }
    }
    size_t n = strlen(in);
    while (n > 0 && isspace((unsigned char)in[n - 1]))
        n--;
    if (n == 0 || in[0] == '"' || strchr(in, '[') || !plc_software) {
        snprintf(out, cap, "%.*s", (int)n, in);
        return;
    }
    char name[256];
    snprintf(name, sizeof name, "%.*s", (int)(n < sizeof name ? n : sizeof name - 1), in);
    if (is_named_type(plc_software, name))
        snprintf(out, cap, "\"%s\"", name);
    else
        snprintf(out, cap, "%s", name);
}

mxml_node_t *mb_new_member(const char *name, const char *datatype, const char *start_value, const char *comment,
                           const char *lang)
{
    mxml_node_t *m = mxmlNewElement(NULL, "Member");
    mxmlElementSetAttr(m, "Name", name);
    mxmlElementSetAttr(m, "Datatype", datatype);
    if (comment && *comment)
        mb_set_comment(m, lang, comment);
    if (start_value && *start_value)
        mb_set_start_value(m, start_value);
    return m;
}

void mb_set_comment(mxml_node_t *member, const char *lang, const char *text)
{
    mxml_node_t *cm = sml_child(member, "Comment");
    if (!text || !*text) {
        if (cm)
            mxmlDelete(cm);
        return;
    }
    if (!cm) {
        cm = mxmlNewElement(NULL, "Comment");
        mxml_node_t *al = sml_child(member, "AttributeList");
        if (al)
            mxmlAdd(member, MXML_ADD_AFTER, al, cm);
        else
            mxmlAdd(member, MXML_ADD_BEFORE, NULL, cm);
    }
    const char *l = lang && *lang ? lang : "en-US";
    mxml_node_t *t;
    for (t = sml_child(cm, "MultiLanguageText"); t; t = sml_next(t, "MultiLanguageText")) {
        const char *tl = sml_attr(t, "Lang");
        if (tl && _stricmp(tl, l) == 0)
            break;
    }
    if (!t) {
        t = mxmlNewElement(cm, "MultiLanguageText");
        mxmlElementSetAttr(t, "Lang", l);
    }
    sml_set_text(t, text);
}

void mb_set_start_value(mxml_node_t *member, const char *value)
{
    mxml_node_t *sv = sml_child(member, "StartValue");
    if (!value || !*value) {
        if (sv)
            mxmlDelete(sv);
        return;
    }
    if (!sv)
        sv = mxmlNewElement(member, "StartValue");
    sml_set_text(sv, value);
}

void mb_set_element_start_value(mxml_node_t *member, const char *index, const char *value)
{
    mxml_node_t *se;
    for (se = sml_child(member, "Subelement"); se; se = sml_next(se, "Subelement")) {
        const char *p = sml_attr(se, "Path");
        if (p && strcmp(p, index) == 0)
            break;
    }
    if (!se) {
        se = mxmlNewElement(member, "Subelement");
        mxmlElementSetAttr(se, "Path", index);
    }
    mb_set_start_value(se, value);
}

mxml_node_t *mb_section(mxml_node_t *sections, const char *name)
{
    for (mxml_node_t *s = sml_child(sections, "Section"); s; s = sml_next(s, "Section")) {
        const char *n = sml_attr(s, "Name");
        if (n && _stricmp(n, name) == 0)
            return s;
    }
    return NULL;
}

mxml_node_t *mb_parent_for(tool_ctx *c, mxml_node_t *sections, const char *section, const char *path, const char **leaf)
{
    const char *dot = strrchr(path, '.');
    *leaf = dot ? dot + 1 : path;
    mxml_node_t *sec = mb_section(sections, section);
    if (!sec) {
        fail(c, "section '%s' not found", section);
        return NULL;
    }
    if (!dot)
        return sec;
    char parent_path[512];
    snprintf(parent_path, sizeof parent_path, "%.*s", (int)(dot - path), path);
    const char *found_section = NULL;
    mxml_node_t *parent = sml_find_member(sections, section, parent_path, &found_section);
    if (!parent) {
        fail(c, "parent member '%s' not found in section %s", parent_path, section);
        return NULL;
    }
    const char *dt = sml_attr(parent, "Datatype");
    if (!dt || _stricmp(dt, "Struct") != 0) {
        fail(c, "'%s' is of type %s: members can only be added inside a Struct (edit the UDT itself for UDT types)",
             parent_path, dt ? dt : "?");
        return NULL;
    }
    return parent;
}

void mb_describe_members(mxml_node_t *sections, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
    for (mxml_node_t *s = sml_child(sections, "Section"); s && n < cap; s = sml_next(s, "Section")) {
        n += (size_t)snprintf(out + n, cap - n, "%s%s: ", n ? "; " : "", sml_attr(s, "Name") ? sml_attr(s, "Name") : "?");
        int first = 1;
        for (mxml_node_t *m = sml_child(s, "Member"); m && n < cap; m = sml_next(m, "Member")) {
            n += (size_t)snprintf(out + n, cap - n, "%s%s", first ? "" : ", ", sml_attr(m, "Name") ? sml_attr(m, "Name") : "?");
            first = 0;
        }
        if (first && n < cap)
            n += (size_t)snprintf(out + n, cap - n, "(empty)");
    }
}

th mb_import_file(tool_ctx *c, th group, const char *collection, const char *path, int override)
{
    th coll = td_get_h(group, collection);
    if (!coll) {
        fail_td(c, "cannot access the target folder");
        return 0;
    }
    cJSON *res = td_call(coll, "Import", tda("fe", path, "Siemens.Engineering.ImportOptions", override ? "Override" : "None"));
    if (!res) {
        fail_td(c, "import failed");
        return 0;
    }
    th first = 0;
    if (tdv_h(res)) {
        cJSON *items = td_enum(tdv_h(res), "Name", -1);
        first = tdv_h(cJSON_GetArrayItem(items, 0));
        cJSON_Delete(items);
    }
    cJSON_Delete(res);
    return first;
}

th mb_reimport(tool_ctx *c, th obj, const char *collection, mxml_node_t *top)
{
    th parent = td_get_h(obj, "Parent");
    if (!parent) {
        fail_td(c, "cannot find the owning folder");
        return 0;
    }
    char path[1024];
    if (fs_temp_path("import", ".xml", path, sizeof path) != 0 || sml_save_file(top, path) != 0) {
        fail(c, "cannot write the temporary XML");
        return 0;
    }
    th res = mb_import_file(c, parent, collection, path, 1);
    if (res)
        fs_remove(path);
    else
        out(c, "(the rejected XML was kept in %s)\n", path);
    return res;
}

/* ---- editing session ------------------------------------------------------------- */

int ed_open(tool_ctx *c, th project, th item, const char *collection, ed_doc *d)
{
    memset(d, 0, sizeof *d);
    d->item = item;
    d->collection = collection;
    snprintf(d->culture, sizeof d->culture, "%s", mb_editing_language(project));
    d->top = sw_export_tree(c, item);
    if (!d->top)
        return -1;
    d->obj = sml_object(d->top);
    d->sections = sml_sections(d->obj);
    if (!d->obj) {
        ed_close(d);
        return fail(c, "unexpected export format");
    }
    return 0;
}

void ed_close(ed_doc *d)
{
    if (d->top)
        mxmlDelete(d->top);
    d->top = NULL;
}

th ed_commit(tool_ctx *c, ed_doc *d)
{
    th res = mb_reimport(c, d->item, d->collection, d->top);
    ed_close(d);
    return res;
}

mxml_node_t *ed_member(tool_ctx *c, ed_doc *d, const char *section, const char *path, const char **found_section)
{
    mxml_node_t *m = d->sections ? sml_find_member(d->sections, section, path, found_section) : NULL;
    if (!m) {
        char list[2048];
        mb_describe_members(d->sections, list, sizeof list);
        fail(c, "member '%s' not found%s%s. Members: %s", path, section && *section ? " in section " : "",
             section && *section ? section : "", list);
    }
    return m;
}

int ed_add_member(tool_ctx *c, ed_doc *d, const char *section, const char *path, const char *datatype,
                  const char *start, const char *comment)
{
    if (!d->sections)
        return fail(c, "this object has no interface");
    const char *existing_section = NULL;
    if (sml_find_member(d->sections, section, path, &existing_section))
        return fail(c, "member '%s' already exists in section %s", path, existing_section);
    const char *leaf_name = NULL;
    mxml_node_t *parent = mb_parent_for(c, d->sections, section, path, &leaf_name);
    if (!parent)
        return -1;
    mxml_node_t *m = mb_new_member(leaf_name, datatype, start, comment, d->culture);
    mxmlAdd(parent, MXML_ADD_AFTER, NULL, m);
    return 0;
}

int ed_update_member(tool_ctx *c, ed_doc *d, mxml_node_t *member, const char *new_name, const char *new_type,
                     const char *new_start, const char *new_comment)
{
    (void)c;
    if (new_name && *new_name)
        mxmlElementSetAttr(member, "Name", new_name);
    if (new_type && *new_type) {
        mxmlElementSetAttr(member, "Datatype", new_type);
        /* Children of a former Struct do not belong to the new type. */
        if (_stricmp(new_type, "Struct") != 0) {
            mxml_node_t *ch;
            while ((ch = sml_child(member, "Member")) != NULL)
                mxmlDelete(ch);
            if ((ch = sml_child(member, "Sections")) != NULL)
                mxmlDelete(ch);
        }
    }
    if (new_start)
        mb_set_start_value(member, new_start);
    if (new_comment)
        mb_set_comment(member, d->culture, new_comment);
    return 0;
}

th mb_import_tree(tool_ctx *c, th group, const char *collection, mxml_node_t *top, int override)
{
    char path[1024];
    if (fs_temp_path("import", ".xml", path, sizeof path) != 0 || sml_save_file(top, path) != 0) {
        fail(c, "cannot write the temporary XML");
        return 0;
    }
    th res = mb_import_file(c, group, collection, path, override);
    if (res)
        fs_remove(path);
    else
        out(c, "(the rejected XML was kept in %s)\n", path);
    return res;
}

th mb_generate_from_source(tool_ctx *c, th plc_software, const char *name, const char *ext, const char *text,
                           th target_group)
{
    char dir[1024], path[1024], file[300], src_name[300];
    if (fs_temp_dir("source", dir, sizeof dir) != 0) {
        fail(c, "cannot create a temporary folder");
        return 0;
    }
    snprintf(file, sizeof file, "%s%s", name, ext);
    for (char *p = file; *p; p++)
        if (strchr("\\/:*?\"<>|", *p))
            *p = '_';
    fs_join(path, sizeof path, dir, file);
    if (fs_write_all(path, text, strlen(text)) != 0) {
        fail(c, "cannot write the temporary source");
        return 0;
    }
    snprintf(src_name, sizeof src_name, "tiacomandante_%s", file);
    th group = td_get_h(plc_software, "ExternalSourceGroup");
    th sources = group ? td_get_h(group, "ExternalSources") : 0;
    th old = sources ? td_call_h(sources, "Find", tda("s", src_name)) : 0;
    if (old)
        td_call_v(old, "Delete", NULL);
    td_clear_err();
    th src = sources ? td_call_h(sources, "CreateFromFile", tda("ss", src_name, path)) : 0;
    if (!src) {
        fail_td(c, "creating the external source failed");
        return 0;
    }
    cJSON *res;
    if (target_group)
        res = td_call(src, "GenerateBlocksFromSource",
                      tda("he", target_group, "Siemens.Engineering.SW.ExternalSources.GenerateBlockOption", "None"));
    else
        res = td_call(src, "GenerateBlocksFromSource", tda("e", "Siemens.Engineering.SW.ExternalSources.GenerateBlockOption", "None"));
    th first = 0;
    if (!res) {
        fail_td(c, "compiling the source failed");
        out(c, "Generated source (%s):\n%s\n", file, text);
    } else {
        cJSON *items = tdv_h(res) ? td_enum(tdv_h(res), "Name", -1) : NULL;
        first = tdv_h(cJSON_GetArrayItem(items, 0));
        cJSON_Delete(items);
        cJSON_Delete(res);
    }
    td_call_v(src, "Delete", NULL);
    fs_remove(path);
    fs_remove_tree(dir);
    return first;
}

static th ml_item(th obj, const char *prop, const char *culture)
{
    th mt = td_get_h(obj, prop);
    th items = mt ? td_get_h(mt, "Items") : 0;
    cJSON *list = items ? td_enum(items, NULL, -1) : NULL;
    th found = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        th lang = td_get_h(tdv_h(it), "Language");
        char *cult = lang ? td_get_s(lang, "Culture") : NULL;
        int match = cult && _stricmp(cult, culture) == 0;
        free(cult);
        if (match) {
            found = tdv_h(it);
            break;
        }
    }
    cJSON_Delete(list);
    td_clear_err();
    return found;
}

int mb_ml_set(th obj, const char *prop, const char *culture, const char *text)
{
    th item = ml_item(obj, prop, culture);
    if (!item)
        return -1;
    return td_set(item, "Text", cJSON_CreateString(text ? text : ""));
}

char *mb_ml_get(th obj, const char *prop, const char *culture)
{
    th item = ml_item(obj, prop, culture);
    return item ? td_get_s(item, "Text") : NULL;
}

th mb_move(tool_ctx *c, th item, const char *collection, th target_group)
{
    char path[1024];
    if (sw_export_xml(c, item, path, sizeof path) != 0)
        return 0;
    th parent = td_get_h(item, "Parent");
    if (!parent) {
        fs_remove(path);
        fail_td(c, "cannot find the current folder");
        return 0;
    }
    td_pin(parent);
    if (td_call_v(item, "Delete", NULL) != 0) {
        td_unpin(parent);
        fs_remove(path);
        fail_td(c, "removing the object from its folder failed");
        return 0;
    }
    th res = mb_import_file(c, target_group, collection, path, 0);
    if (!res) {
        tool_ctx probe;
        ctx_init(&probe, c->tool, NULL);
        th back = mb_import_file(&probe, parent, collection, path, 0);
        ctx_free(&probe);
        out(c, back ? "The object was restored in its original folder.\n"
                    : "RESTORE FAILED: the exported XML is kept in %s - import it manually.\n", path);
        if (back)
            fs_remove(path);
    } else {
        fs_remove(path);
    }
    td_unpin(parent);
    return res;
}
