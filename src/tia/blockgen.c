#include "blockgen.h"

#include "tia/members.h"
#include "util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_INTERFACE "http://www.siemens.com/automation/Openness/SW/Interface/v5"

static const char *or_default(const char *s, const char *def)
{
    return s && *s ? s : def;
}

static int is_code_block(const char *kind)
{
    return strcmp(kind, "FB") == 0 || strcmp(kind, "FC") == 0 || strcmp(kind, "OB") == 0;
}

/* ---- SimaticML ------------------------------------------------------------- */

typedef struct idgen {
    int next;
} idgen;

static void set_id(mxml_node_t *n, idgen *g)
{
    mxmlElementSetAttrf(n, "ID", "%X", g->next++);
}

static mxml_node_t *leaf(mxml_node_t *parent, const char *name, const char *text)
{
    mxml_node_t *e = mxmlNewElement(parent, name);
    if (text && *text)
        mxmlNewOpaque(e, text);
    return e;
}

static void ml_text(mxml_node_t *object_list, const char *composition, const char *culture, const char *text, idgen *g)
{
    mxml_node_t *mt = mxmlNewElement(object_list, "MultilingualText");
    set_id(mt, g);
    mxmlElementSetAttr(mt, "CompositionName", composition);
    mxml_node_t *ol = mxmlNewElement(mt, "ObjectList");
    mxml_node_t *item = mxmlNewElement(ol, "MultilingualTextItem");
    set_id(item, g);
    mxmlElementSetAttr(item, "CompositionName", "Items");
    mxml_node_t *al = mxmlNewElement(item, "AttributeList");
    leaf(al, "Culture", culture);
    leaf(al, "Text", text);
}

static const char *const *sections_for(const char *kind, int *n)
{
    static const char *fb[] = { "Input", "Output", "InOut", "Static", "Temp", "Constant" };
    static const char *fc[] = { "Input", "Output", "InOut", "Temp", "Constant", "Return" };
    static const char *ob[] = { "Input", "Temp", "Constant" };
    static const char *db[] = { "Static" };
    static const char *udt[] = { "None" };
    if (strcmp(kind, "FB") == 0) {
        *n = 6;
        return fb;
    }
    if (strcmp(kind, "FC") == 0) {
        *n = 6;
        return fc;
    }
    if (strcmp(kind, "OB") == 0) {
        *n = 3;
        return ob;
    }
    if (strcmp(kind, "PlcStruct") == 0) {
        *n = 1;
        return udt;
    }
    *n = 1;
    return db;
}

static void add_members(mxml_node_t *section, const bg_spec *s, const char *name)
{
    for (int i = 0; i < s->nmembers; i++) {
        const bg_member *m = &s->members[i];
        if (_stricmp(m->section, name) != 0)
            continue;
        /* Dotted names create members inside an existing Struct member. */
        mxml_node_t *parent = section;
        char path[128];
        snprintf(path, sizeof path, "%s", m->name);
        char *seg = path, *dot;
        while ((dot = strchr(seg, '.')) != NULL) {
            *dot = 0;
            mxml_node_t *found = NULL;
            for (mxml_node_t *c = mxmlGetFirstChild(parent); c; c = mxmlGetNextSibling(c)) {
                if (mxmlGetType(c) == MXML_TYPE_ELEMENT && strcmp(mxmlGetElement(c), "Member") == 0 &&
                    mxmlElementGetAttr(c, "Name") && _stricmp(mxmlElementGetAttr(c, "Name"), seg) == 0) {
                    found = c;
                    break;
                }
            }
            if (!found) {
                found = mb_new_member(seg, "Struct", NULL, NULL, s->culture);
                mxmlAdd(parent, MXML_ADD_AFTER, NULL, found);
            }
            parent = found;
            seg = dot + 1;
        }
        mxml_node_t *mem = mb_new_member(seg, m->type, m->start, m->comment, or_default(s->culture, "en-US"));
        mxmlAdd(parent, MXML_ADD_AFTER, NULL, mem);
    }
}

static int section_has_member(const bg_spec *s, const char *section)
{
    for (int i = 0; i < s->nmembers; i++)
        if (_stricmp(s->members[i].section, section) == 0)
            return 1;
    return 0;
}

mxml_node_t *bg_build_xml(const bg_spec *s)
{
    idgen g = { 0 };
    const char *culture = or_default(s->culture, "en-US");
    mxml_node_t *top = mxmlNewXML("1.0");
    mxml_node_t *doc = mxmlNewElement(top, "Document");
    mxml_node_t *eng = mxmlNewElement(doc, "Engineering");
    mxmlElementSetAttr(eng, "version", "V21");

    char elem[64];
    snprintf(elem, sizeof elem, "%s.%s", strcmp(s->kind, "PlcStruct") == 0 ? "SW.Types" : "SW.Blocks", s->kind);
    mxml_node_t *obj = mxmlNewElement(doc, elem);
    set_id(obj, &g);
    mxml_node_t *al = mxmlNewElement(obj, "AttributeList");
    int is_udt = strcmp(s->kind, "PlcStruct") == 0;
    if (!is_udt) {
        leaf(al, "AutoNumber", s->number > 0 ? "false" : "true");
        leaf(al, "HeaderAuthor", s->author);
        leaf(al, "HeaderFamily", s->family);
        leaf(al, "HeaderName", NULL);
        leaf(al, "HeaderVersion", or_default(s->version, "0.1"));
    }
    mxml_node_t *iface = mxmlNewElement(al, "Interface");
    mxml_node_t *sections = mxmlNewElement(iface, "Sections");
    mxmlElementSetAttr(sections, "xmlns", NS_INTERFACE);
    int nsec = 0;
    const char *const *secs = sections_for(s->kind, &nsec);
    for (int i = 0; i < nsec; i++) {
        mxml_node_t *sec = mxmlNewElement(sections, "Section");
        mxmlElementSetAttr(sec, "Name", secs[i]);
        if (strcmp(secs[i], "Return") == 0) {
            mxml_node_t *ret = mxmlNewElement(sec, "Member");
            mxmlElementSetAttr(ret, "Name", "Ret_Val");
            mxmlElementSetAttr(ret, "Datatype", or_default(s->return_type, "Void"));
            continue;
        }
        if (strcmp(s->kind, "OB") == 0 && strcmp(secs[i], "Input") == 0 && !section_has_member(s, "Input") &&
            strcmp(or_default(s->secondary_type, "ProgramCycle"), "ProgramCycle") == 0) {
            mxml_node_t *m1 = mb_new_member("Initial_Call", "Bool", NULL, "Initial call of this OB", culture);
            mxmlAdd(sec, MXML_ADD_AFTER, NULL, m1);
            mxml_node_t *m2 = mb_new_member("Remanence", "Bool", NULL, "=True, if remanent data are available", culture);
            mxmlAdd(sec, MXML_ADD_AFTER, NULL, m2);
            mxmlElementSetAttr(m1, "Informative", "true");
            mxmlElementSetAttr(m2, "Informative", "true");
        }
        add_members(sec, s, secs[i]);
    }
    if (strcmp(s->kind, "PlcStruct") != 0)
        leaf(al, "MemoryLayout", or_default(s->memory_layout, "Optimized"));
    leaf(al, "Name", s->name);
    leaf(al, "Namespace", NULL); /* identifier attribute required by V21 (software units) */
    if (!is_udt) {
        if (s->number > 0) {
            char num[32];
            snprintf(num, sizeof num, "%lld", s->number);
            leaf(al, "Number", num);
        }
        leaf(al, "ProgrammingLanguage", or_default(s->language, is_code_block(s->kind) ? "LAD" : "DB"));
        if (strcmp(s->kind, "OB") == 0)
            leaf(al, "SecondaryType", or_default(s->secondary_type, "ProgramCycle"));
    }

    mxml_node_t *ol = mxmlNewElement(obj, "ObjectList");
    ml_text(ol, "Comment", culture, s->comment, &g);
    if (is_code_block(s->kind)) {
        int nnet = s->networks > 0 ? s->networks : 1;
        const char *lang = or_default(s->language, "LAD");
        for (int i = 0; i < nnet; i++) {
            mxml_node_t *cu = mxmlNewElement(ol, "SW.Blocks.CompileUnit");
            set_id(cu, &g);
            mxmlElementSetAttr(cu, "CompositionName", "CompileUnits");
            mxml_node_t *cal = mxmlNewElement(cu, "AttributeList");
            mxmlNewElement(cal, "NetworkSource");
            leaf(cal, "ProgrammingLanguage", lang);
            mxml_node_t *col = mxmlNewElement(cu, "ObjectList");
            ml_text(col, "Comment", culture, NULL, &g);
            ml_text(col, "Title", culture, s->network_titles ? s->network_titles[i] : NULL, &g);
        }
    }
    ml_text(ol, "Title", culture, s->title, &g);
    return top;
}

/* ---- SCL source ----------------------------------------------------------------- */

static void scl_quote(strbuf *sb, const char *s)
{
    sb_appendc(sb, '\'');
    for (; *s; s++) {
        if (*s == '\'')
            sb_appendc(sb, '$');
        sb_appendc(sb, *s);
    }
    sb_appendc(sb, '\'');
}

static void scl_members(strbuf *sb, const bg_spec *s, const char *section, const char *keyword)
{
    if (!section_has_member(s, section))
        return;
    sb_printf(sb, "   %s\n", keyword);
    for (int i = 0; i < s->nmembers; i++) {
        const bg_member *m = &s->members[i];
        if (_stricmp(m->section, section) != 0)
            continue;
        sb_printf(sb, "      %s : %s", m->name, m->type);
        if (m->start[0])
            sb_printf(sb, " := %s", m->start);
        sb_append(sb, ";");
        if (m->comment[0]) {
            sb_append(sb, "   // ");
            for (const char *p = m->comment; *p; p++)
                sb_appendc(sb, *p == '\n' || *p == '\r' ? ' ' : *p);
        }
        sb_append(sb, "\n");
    }
    sb_append(sb, "   END_VAR\n\n");
}

char *bg_build_scl(const bg_spec *s, const char *code)
{
    strbuf sb;
    sb_init(&sb);
    const char *kind = s->kind;
    const char *head, *tail;
    if (strcmp(kind, "FB") == 0) {
        head = "FUNCTION_BLOCK";
        tail = "END_FUNCTION_BLOCK";
    } else if (strcmp(kind, "FC") == 0) {
        head = "FUNCTION";
        tail = "END_FUNCTION";
    } else if (strcmp(kind, "OB") == 0) {
        head = "ORGANIZATION_BLOCK";
        tail = "END_ORGANIZATION_BLOCK";
    } else if (strcmp(kind, "PlcStruct") == 0) {
        head = "TYPE";
        tail = "END_TYPE";
    } else {
        head = "DATA_BLOCK";
        tail = "END_DATA_BLOCK";
    }
    sb_printf(&sb, "%s \"%s\"", head, s->name);
    if (strcmp(kind, "FC") == 0)
        sb_printf(&sb, " : %s", or_default(s->return_type, "Void"));
    sb_append(&sb, "\n");
    if (s->title && *s->title) {
        sb_append(&sb, "TITLE = ");
        for (const char *p = s->title; *p; p++)
            sb_appendc(&sb, *p == '\n' || *p == '\r' ? ' ' : *p);
        sb_append(&sb, "\n");
    }
    if (strcmp(kind, "PlcStruct") != 0)
        sb_printf(&sb, "{ S7_Optimized_Access := '%s' }\n",
                  s->memory_layout && _stricmp(s->memory_layout, "Standard") == 0 ? "FALSE" : "TRUE");
    if (s->author && *s->author) {
        sb_append(&sb, "AUTHOR : ");
        scl_quote(&sb, s->author);
        sb_append(&sb, "\n");
    }
    if (s->family && *s->family) {
        sb_append(&sb, "FAMILY : ");
        scl_quote(&sb, s->family);
        sb_append(&sb, "\n");
    }
    sb_printf(&sb, "VERSION : %s\n", or_default(s->version, "0.1"));
    if (strcmp(kind, "PlcStruct") == 0) {
        sb_append(&sb, "   STRUCT\n");
        for (int i = 0; i < s->nmembers; i++) {
            const bg_member *m = &s->members[i];
            sb_printf(&sb, "      %s : %s%s%s;%s%s\n", m->name, m->type, m->start[0] ? " := " : "", m->start,
                      m->comment[0] ? "   // " : "", m->comment);
        }
        sb_append(&sb, "   END_STRUCT;\n\nEND_TYPE\n");
        return sb_detach(&sb);
    }
    if (strcmp(kind, "FB") == 0 || strcmp(kind, "FC") == 0) {
        scl_members(&sb, s, "Input", "VAR_INPUT");
        scl_members(&sb, s, "Output", "VAR_OUTPUT");
        scl_members(&sb, s, "InOut", "VAR_IN_OUT");
    }
    if (strcmp(kind, "OB") == 0)
        scl_members(&sb, s, "Input", "VAR_INPUT");
    if (strcmp(kind, "FB") == 0 || strcmp(kind, "GlobalDB") == 0)
        scl_members(&sb, s, strcmp(kind, "FB") == 0 ? "Static" : "Static", "VAR");
    if (strcmp(kind, "GlobalDB") != 0) {
        scl_members(&sb, s, "Temp", "VAR_TEMP");
        scl_members(&sb, s, "Constant", "VAR CONSTANT");
    }
    sb_append(&sb, "\nBEGIN\n");
    if (code && *code) {
        sb_append(&sb, code);
        if (code[strlen(code) - 1] != '\n')
            sb_append(&sb, "\n");
    }
    sb_printf(&sb, "%s\n", tail);
    (void)tail;
    return sb_detach(&sb);
}

/* ---- interface argument ------------------------------------------------------------ */

static const char *pick(const cJSON *o, const char *const *keys)
{
    for (; *keys; keys++) {
        const cJSON *v = cJSON_GetObjectItem(o, *keys); /* case-insensitive */
        if (cJSON_IsString(v))
            return v->valuestring;
        if (cJSON_IsNumber(v)) {
            static char buf[64];
            if (v->valuedouble == (double)(long long)v->valuedouble)
                snprintf(buf, sizeof buf, "%lld", (long long)v->valuedouble);
            else
                snprintf(buf, sizeof buf, "%.15g", v->valuedouble);
            return buf;
        }
        if (cJSON_IsBool(v))
            return cJSON_IsTrue(v) ? "TRUE" : "FALSE";
    }
    return NULL;
}

static const char *canonical_section(const char *s)
{
    static const char *names[] = { "Input", "Output", "InOut", "Static", "Temp", "Constant", "Return", "None" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (_stricmp(s, names[i]) == 0)
            return names[i];
    if (_stricmp(s, "In_Out") == 0 || _stricmp(s, "InOuts") == 0)
        return "InOut";
    if (_stricmp(s, "Inputs") == 0)
        return "Input";
    if (_stricmp(s, "Outputs") == 0)
        return "Output";
    if (_stricmp(s, "Var") == 0 || _stricmp(s, "Stat") == 0)
        return "Static";
    return NULL;
}

static int add_entry(tool_ctx *c, const cJSON *e, const char *section, th plc, bg_member **out, int *n, int *cap)
{
    static const char *k_name[] = { "name", "memberName", NULL };
    static const char *k_type[] = { "dataType", "datatype", "type", NULL };
    static const char *k_start[] = { "startValue", "initialValue", "start", "defaultValue", NULL };
    static const char *k_comment[] = { "comment", NULL };
    static const char *k_section[] = { "section", NULL };
    const char *name = pick(e, k_name);
    const char *type = pick(e, k_type);
    const char *sec = pick(e, k_section);
    if (sec)
        section = sec;
    if (!name || !type)
        return fail(c, "interface entries need name and dataType");
    const char *cs = canonical_section(section ? section : "");
    if (!cs)
        return fail(c, "unknown interface section '%s' (Input, Output, InOut, Static, Temp, Constant)", section ? section : "");
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        bg_member *p = realloc(*out, (size_t)*cap * sizeof *p);
        if (!p)
            return fail(c, "out of memory");
        *out = p;
    }
    bg_member *m = &(*out)[(*n)++];
    memset(m, 0, sizeof *m);
    snprintf(m->section, sizeof m->section, "%s", cs);
    snprintf(m->name, sizeof m->name, "%s", name);
    mb_normalize_datatype(plc, type, m->type, sizeof m->type);
    const char *start = pick(e, k_start);
    const char *comment = pick(e, k_comment);
    snprintf(m->start, sizeof m->start, "%s", start ? start : "");
    snprintf(m->comment, sizeof m->comment, "%s", comment ? comment : "");
    return 0;
}

int bg_parse_interface(tool_ctx *c, const cJSON *iface, th plc_software, const char *default_section, bg_member **out,
                       int *n)
{
    *out = NULL;
    *n = 0;
    int cap = 0;
    if (!iface)
        return 0;
    if (cJSON_IsArray(iface)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, iface)
        {
            if (add_entry(c, e, default_section, plc_software, out, n, &cap) != 0)
                return -1;
        }
        return 0;
    }
    if (!cJSON_IsObject(iface))
        return fail(c, "interface must be an object {Section: [members]} or an array of members");
    const cJSON *sec;
    cJSON_ArrayForEach(sec, iface)
    {
        if (!cJSON_IsArray(sec))
            return fail(c, "interface section '%s' must be an array of members", sec->string);
        const cJSON *e;
        cJSON_ArrayForEach(e, sec)
        {
            if (add_entry(c, e, sec->string, plc_software, out, n, &cap) != 0)
                return -1;
        }
    }
    return 0;
}
