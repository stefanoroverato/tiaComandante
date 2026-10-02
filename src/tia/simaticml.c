#include "simaticml.h"

#include "util/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mxml_options_t *load_options(void)
{
    mxml_options_t *o = mxmlOptionsNew();
    mxmlOptionsSetTypeValue(o, MXML_TYPE_OPAQUE);
    return o;
}

mxml_node_t *sml_load_string(const char *text, char *err, size_t errlen)
{
    if (!text) {
        snprintf(err, errlen, "no XML content");
        return NULL;
    }
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text += 3;
    mxml_options_t *o = load_options();
    mxml_node_t *top = mxmlLoadString(NULL, o, text);
    mxmlOptionsDelete(o);
    if (!top)
        snprintf(err, errlen, "malformed XML");
    return top;
}

mxml_node_t *sml_load_file(const char *path, char *err, size_t errlen)
{
    char *data = NULL;
    if (fs_read_all(path, &data, NULL) != 0) {
        snprintf(err, errlen, "cannot read %s", path);
        return NULL;
    }
    mxml_node_t *top = sml_load_string(data, err, errlen);
    free(data);
    return top;
}

char *sml_save_string(mxml_node_t *top)
{
    mxml_options_t *o = mxmlOptionsNew();
    char *s = mxmlSaveAllocString(top, o);
    mxmlOptionsDelete(o);
    return s;
}

int sml_save_file(mxml_node_t *top, const char *path)
{
    char *s = sml_save_string(top);
    if (!s)
        return -1;
    int rc = fs_write_all(path, s, strlen(s));
    free(s);
    return rc;
}

static int is_element(mxml_node_t *n, const char *name)
{
    if (!n || mxmlGetType(n) != MXML_TYPE_ELEMENT)
        return 0;
    if (!name)
        return 1;
    const char *e = mxmlGetElement(n);
    return e && strcmp(e, name) == 0;
}

mxml_node_t *sml_child(mxml_node_t *n, const char *name)
{
    for (mxml_node_t *c = n ? mxmlGetFirstChild(n) : NULL; c; c = mxmlGetNextSibling(c))
        if (is_element(c, name))
            return c;
    return NULL;
}

mxml_node_t *sml_next(mxml_node_t *n, const char *name)
{
    for (mxml_node_t *c = n ? mxmlGetNextSibling(n) : NULL; c; c = mxmlGetNextSibling(c))
        if (is_element(c, name))
            return c;
    return NULL;
}

mxml_node_t *sml_find(mxml_node_t *top, const char *name)
{
    return top ? mxmlFindElement(top, top, name, NULL, NULL, MXML_DESCEND_ALL) : NULL;
}

mxml_node_t *sml_path(mxml_node_t *n, const char *path)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    char *ctx = NULL;
    for (char *seg = strtok_s(buf, "/", &ctx); seg && n; seg = strtok_s(NULL, "/", &ctx))
        n = sml_child(n, seg);
    return n;
}

const char *sml_text(mxml_node_t *n)
{
    if (!n)
        return "";
    for (mxml_node_t *c = mxmlGetFirstChild(n); c; c = mxmlGetNextSibling(c)) {
        if (mxmlGetType(c) == MXML_TYPE_OPAQUE) {
            const char *s = mxmlGetOpaque(c);
            return s ? s : "";
        }
        if (mxmlGetType(c) == MXML_TYPE_CDATA) {
            const char *s = mxmlGetCDATA(c);
            return s ? s : "";
        }
    }
    return "";
}

const char *sml_child_text(mxml_node_t *n, const char *name)
{
    mxml_node_t *c = sml_child(n, name);
    return c ? sml_text(c) : NULL;
}

const char *sml_attr(mxml_node_t *n, const char *name)
{
    return n ? mxmlElementGetAttr(n, name) : NULL;
}

void sml_set_text(mxml_node_t *n, const char *text)
{
    mxml_node_t *c;
    while ((c = mxmlGetFirstChild(n)) != NULL)
        mxmlDelete(c);
    if (text && *text)
        mxmlNewOpaque(n, text);
}

mxml_node_t *sml_ensure_child(mxml_node_t *n, const char *name, int before_first)
{
    mxml_node_t *c = sml_child(n, name);
    if (c)
        return c;
    c = mxmlNewElement(NULL, name);
    mxmlAdd(n, before_first ? MXML_ADD_BEFORE : MXML_ADD_AFTER, NULL, c);
    return c;
}

mxml_node_t *sml_object(mxml_node_t *top)
{
    mxml_node_t *doc = is_element(top, "Document") ? top : sml_find(top, "Document");
    for (mxml_node_t *c = sml_child(doc, NULL); c; c = sml_next(c, NULL)) {
        const char *e = mxmlGetElement(c);
        if (strcmp(e, "Engineering") != 0 && strcmp(e, "DocumentInfo") != 0)
            return c;
    }
    return NULL;
}

mxml_node_t *sml_attribute_list(mxml_node_t *obj)
{
    return sml_child(obj, "AttributeList");
}

mxml_node_t *sml_sections(mxml_node_t *obj)
{
    return sml_path(obj, "AttributeList/Interface/Sections");
}

const char *sml_ml_text(mxml_node_t *obj, const char *composition, const char *lang)
{
    mxml_node_t *ol = sml_child(obj, "ObjectList");
    for (mxml_node_t *mt = sml_child(ol, "MultilingualText"); mt; mt = sml_next(mt, "MultilingualText")) {
        const char *cn = sml_attr(mt, "CompositionName");
        if (!cn || strcmp(cn, composition) != 0)
            continue;
        const char *first = NULL;
        mxml_node_t *items = sml_child(mt, "ObjectList");
        for (mxml_node_t *it = sml_child(items, "MultilingualTextItem"); it; it = sml_next(it, "MultilingualTextItem")) {
            mxml_node_t *al = sml_child(it, "AttributeList");
            const char *culture = sml_child_text(al, "Culture");
            const char *text = sml_child_text(al, "Text");
            if (!text || !*text)
                continue;
            if (lang && culture && _stricmp(culture, lang) == 0)
                return text;
            if (!first)
                first = text;
        }
        return first;
    }
    return NULL;
}

const char *sml_member_comment(mxml_node_t *member, const char *lang)
{
    mxml_node_t *cm = sml_child(member, "Comment");
    const char *first = NULL;
    for (mxml_node_t *t = sml_child(cm, "MultiLanguageText"); t; t = sml_next(t, "MultiLanguageText")) {
        const char *text = sml_text(t);
        if (!*text)
            continue;
        const char *l = sml_attr(t, "Lang");
        if (lang && l && _stricmp(l, lang) == 0)
            return text;
        if (!first)
            first = text;
    }
    return first;
}

const char *sml_start_value(mxml_node_t *member)
{
    return sml_child_text(member, "StartValue");
}

static int walk_members(mxml_node_t *parent, const char *section, const char *prefix, int depth, int max_depth,
                        sml_member_fn fn, void *ctx)
{
    for (mxml_node_t *m = sml_child(parent, "Member"); m; m = sml_next(m, "Member")) {
        const char *name = sml_attr(m, "Name");
        char path[1024];
        snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "." : "", name ? name : "?");
        int stop = fn(ctx, m, section, path, depth);
        if (stop)
            return stop;
        if (max_depth >= 0 && depth >= max_depth)
            continue;
        /* Nested members: Struct/UDT members directly, multi-instances under Sections/Section. */
        if ((stop = walk_members(m, section, path, depth + 1, max_depth, fn, ctx)) != 0)
            return stop;
        mxml_node_t *inner = sml_child(m, "Sections");
        for (mxml_node_t *s = sml_child(inner, "Section"); s; s = sml_next(s, "Section")) {
            if ((stop = walk_members(s, section, path, depth + 1, max_depth, fn, ctx)) != 0)
                return stop;
        }
    }
    return 0;
}

int sml_walk_members(mxml_node_t *sections, int max_depth, sml_member_fn fn, void *ctx)
{
    for (mxml_node_t *s = sml_child(sections, "Section"); s; s = sml_next(s, "Section")) {
        const char *name = sml_attr(s, "Name");
        int stop = walk_members(s, name ? name : "?", "", 0, max_depth, fn, ctx);
        if (stop)
            return stop;
    }
    return 0;
}

typedef struct find_member_ctx {
    const char *section;
    const char *path;
    mxml_node_t *found;
    const char *found_section;
} find_member_ctx;

static int find_member_cb(void *ctx, mxml_node_t *member, const char *section, const char *path, int depth)
{
    (void)depth;
    find_member_ctx *f = ctx;
    if (f->section && *f->section && _stricmp(section, f->section) != 0)
        return 0;
    if (_stricmp(path, f->path) == 0) {
        f->found = member;
        f->found_section = section;
        return 1;
    }
    return 0;
}

mxml_node_t *sml_find_member(mxml_node_t *sections, const char *section, const char *path, const char **found_section)
{
    find_member_ctx f = { section, path, NULL, NULL };
    sml_walk_members(sections, -1, find_member_cb, &f);
    if (found_section)
        *found_section = f.found_section;
    return f.found;
}

int sml_compile_unit_count(mxml_node_t *obj)
{
    int n = 0;
    mxml_node_t *ol = sml_child(obj, "ObjectList");
    for (mxml_node_t *c = sml_child(ol, "SW.Blocks.CompileUnit"); c; c = sml_next(c, "SW.Blocks.CompileUnit"))
        n++;
    return n;
}

mxml_node_t *sml_compile_unit(mxml_node_t *obj, int index)
{
    mxml_node_t *ol = sml_child(obj, "ObjectList");
    int i = 0;
    for (mxml_node_t *c = sml_child(ol, "SW.Blocks.CompileUnit"); c; c = sml_next(c, "SW.Blocks.CompileUnit"))
        if (i++ == index)
            return c;
    return NULL;
}

/* ---- SCL rendering ------------------------------------------------------------ */

static void render_node(mxml_node_t *n, strbuf *out);

static void render_symbol(mxml_node_t *sym, strbuf *out, int global)
{
    int first = 1, prev_component = 0;
    for (mxml_node_t *c = sml_child(sym, NULL); c; c = sml_next(c, NULL)) {
        const char *e = mxmlGetElement(c);
        if (strcmp(e, "Component") == 0) {
            /* Separators normally come as <Token Text="."/>; add one only when missing. */
            if (prev_component)
                sb_appendc(out, '.');
            prev_component = 1;
            const char *name = sml_attr(c, "Name");
            if (global && first)
                sb_printf(out, "\"%s\"", name ? name : "?");
            else
                sb_append(out, name ? name : "?");
            /* Array indexes: <Access> children inside the component. */
            int idx = 0;
            for (mxml_node_t *a = sml_child(c, "Access"); a; a = sml_next(a, "Access")) {
                sb_append(out, idx++ ? "," : "[");
                render_node(a, out);
            }
            if (idx)
                sb_appendc(out, ']');
            first = 0;
        } else if (strcmp(e, "Token") == 0) {
            const char *t = sml_attr(c, "Text");
            sb_append(out, t ? t : "");
            prev_component = 0;
        }
    }
}

static void render_node(mxml_node_t *n, strbuf *out)
{
    const char *e = mxmlGetElement(n);
    if (strcmp(e, "Token") == 0) {
        const char *t = sml_attr(n, "Text");
        sb_append(out, t ? t : "");
    } else if (strcmp(e, "Blank") == 0) {
        const char *num = sml_attr(n, "Num");
        int k = num ? atoi(num) : 1;
        for (int i = 0; i < (k > 0 ? k : 1) && i < 512; i++)
            sb_appendc(out, ' ');
    } else if (strcmp(e, "NewLine") == 0) {
        const char *num = sml_attr(n, "Num");
        int k = num ? atoi(num) : 1;
        for (int i = 0; i < (k > 0 ? k : 1) && i < 64; i++)
            sb_appendc(out, '\n');
    } else if (strcmp(e, "Text") == 0) {
        sb_append(out, sml_text(n));
    } else if (strcmp(e, "LineComment") == 0) {
        sb_append(out, "//");
        sml_render_scl(n, out);
    } else if (strcmp(e, "Comment") == 0) {
        sb_append(out, "(*");
        sml_render_scl(n, out);
        sb_append(out, "*)");
    } else if (strcmp(e, "Access") == 0) {
        const char *scope = sml_attr(n, "Scope");
        mxml_node_t *sym = sml_child(n, "Symbol");
        mxml_node_t *cst = sml_child(n, "Constant");
        if (sym) {
            if (scope && strcmp(scope, "LocalVariable") == 0)
                sb_appendc(out, '#');
            render_symbol(sym, out, scope && (strcmp(scope, "GlobalVariable") == 0 || strcmp(scope, "GlobalConstant") == 0));
        } else if (cst) {
            const char *v = sml_child_text(cst, "ConstantValue");
            const char *name = sml_attr(cst, "Name");
            if (v)
                sb_append(out, v);
            else if (name)
                sb_printf(out, "%s%s", scope && strcmp(scope, "LocalConstant") == 0 ? "#" : "", name);
        } else {
            sml_render_scl(n, out);
        }
    } else if (strcmp(e, "Parameter") == 0) {
        const char *name = sml_attr(n, "Name");
        sb_append(out, name ? name : "?");
        sml_render_scl(n, out);
    } else {
        /* CallInfo, Instruction, ... : render their children in order. */
        sml_render_scl(n, out);
    }
}

void sml_render_scl(mxml_node_t *structured_text, strbuf *out)
{
    for (mxml_node_t *c = sml_child(structured_text, NULL); c; c = sml_next(c, NULL))
        render_node(c, out);
}

void sml_render_access(mxml_node_t *access, strbuf *out)
{
    render_node(access, out);
}
