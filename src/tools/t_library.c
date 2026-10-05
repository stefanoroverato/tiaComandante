/* library: project library and global libraries - types, versions, master
   copies, folders, instantiation, publishing, update / promote, comparison and
   the lifecycle of global libraries. */
#include "tools.h"

#include "app/config.h"
#include "tia/session.h"
#include "tia/tia_dyn.h"
#include "tia/tia_nav.h"
#include "tia/tia_sw.h"
#include "util/fs.h"
#include "util/glob.h"
#include "util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T_USER_GLOBAL "Siemens.Engineering.Library.UserGlobalLibrary"
#define T_LIB_TYPE "Siemens.Engineering.Library.Types.LibraryType"
#define T_MASTER_COPY "Siemens.Engineering.Library.MasterCopies.MasterCopy"
#define MAX_TREE 1000

static int collect_plc(void *ctx, th device, const cJSON *item, const char *group);

/* ---- libraries ------------------------------------------------------------------------------ */

typedef struct lib_ref {
    th lib;
    int is_project;
    int read_only;
    char name[256];
    char path[1024];
} lib_ref;

static int same_path_ci(const char *a, const char *b)
{
    char x[1024], y[1024];
    if (!a || !b || fs_full_path(a, x, sizeof x) != 0 || fs_full_path(b, y, sizeof y) != 0)
        return 0;
    return _stricmp(x, y) == 0;
}

static void open_names(strbuf *sb)
{
    sb_append(sb, "project");
    cJSON *libs = session_portal() ? td_enum(td_get_h(session_portal(), "GlobalLibraries"), "Name", -1) : NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, libs)
    sb_printf(sb, ", %s", tdi_s(it, "Name") ? tdi_s(it, "Name") : "?");
    cJSON_Delete(libs);
    td_clear_err();
}

/* Open global library by name or path; -1 when not open. */
static int lookup_global(const char *name, lib_ref *out)
{
    memset(out, 0, sizeof *out);
    cJSON *libs = td_enum(td_get_h(session_portal(), "GlobalLibraries"), "Name,Path,IsReadOnly", -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, libs)
    {
        const char *n = tdi_s(it, "Name");
        const char *p = tdi_s(it, "Path");
        if ((n && _stricmp(n, name) == 0) || (p && same_path_ci(p, name))) {
            out->lib = tdv_h(it);
            out->read_only = tdi_b(it, "IsReadOnly", 0);
            snprintf(out->name, sizeof out->name, "%s", n ? n : "?");
            snprintf(out->path, sizeof out->path, "%s", p ? p : "");
            break;
        }
    }
    cJSON_Delete(libs);
    td_clear_err();
    return out->lib ? 0 : -1;
}

/* "project" (or empty) = project library; otherwise the name or path of an open global library. */
static int find_library_named(tool_ctx *c, const char *name, lib_ref *out)
{
    memset(out, 0, sizeof *out);
    if (!name || !*name || _stricmp(name, "project") == 0) {
        if (!session_project())
            return fail(c, "no project is open: the project library needs one (session open/connect)");
        out->lib = td_get_h(session_project(), "ProjectLibrary");
        if (!out->lib)
            return fail_td(c, "cannot read the project library");
        out->is_project = 1;
        snprintf(out->name, sizeof out->name, "project");
        return 0;
    }
    if (lookup_global(name, out) != 0) {
        strbuf sb;
        sb_init(&sb);
        open_names(&sb);
        fail(c, "library '%s' is not open. Open libraries: %s. Open a global library with action=open_global_library.", name,
             sb_str(&sb));
        sb_free(&sb);
        return -1;
    }
    return 0;
}

static int find_library(tool_ctx *c, const char *arg, lib_ref *out)
{
    return find_library_named(c, arg_s(c, arg), out);
}

static int require_writable(tool_ctx *c, const lib_ref *l)
{
    if (l->read_only)
        return fail(c, "library '%s' is open read-only: close it and open it again with openMode=ReadWrite", l->name);
    return 0;
}

/* ---- names, folders, paths ---------------------------------------------------------------------- */

enum { SEC_TYPES = 1, SEC_MC = 2, SEC_ALL = 3 };

static int parse_section(tool_ctx *c, const char *arg, int def)
{
    const char *f = arg_s(c, arg);
    if (!f || !*f)
        return def;
    if (_stricmp(f, "types") == 0)
        return SEC_TYPES;
    if (_stricmp(f, "master_copies") == 0 || _stricmp(f, "mastercopies") == 0)
        return SEC_MC;
    if (_stricmp(f, "all") == 0 || _stricmp(f, "both") == 0)
        return SEC_ALL;
    return def;
}

static th section_root(const lib_ref *l, int sec)
{
    return td_get_h(l->lib, sec == SEC_MC ? "MasterCopyFolder" : "TypeFolder");
}

static const char *section_label(int sec)
{
    return sec == SEC_MC ? "Master copies" : "Types";
}

/* Child of a composition by name (exact first, then case-insensitive). */
static th child_named(th composition, const char *name)
{
    if (!composition || !name)
        return 0;
    th h = td_call_h(composition, "Find", tda("s", name));
    td_clear_err();
    if (h)
        return h;
    cJSON *items = td_enum(composition, "Name", -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        const char *n = tdi_s(it, "Name");
        if (n && _stricmp(n, name) == 0) {
            h = tdv_h(it);
            break;
        }
    }
    cJSON_Delete(items);
    td_clear_err();
    return h;
}

/* Strips an optional "Types/" or "Master copies/" prefix (as printed by get_tree) and sets *sec. */
static const char *strip_section(const char *path, int *sec)
{
    static const struct {
        const char *p;
        int s;
    } pre[] = { { "Types/", SEC_TYPES }, { "Master copies/", SEC_MC }, { "master_copies/", SEC_MC }, { "MasterCopies/", SEC_MC } };
    while (path && (*path == '/' || *path == '\\'))
        path++;
    for (size_t i = 0; path && i < sizeof pre / sizeof pre[0]; i++) {
        size_t n = strlen(pre[i].p);
        if (_strnicmp(path, pre[i].p, n) == 0) {
            *sec = pre[i].s;
            return path + n;
        }
    }
    return path ? path : "";
}

typedef struct lib_obj {
    th h;
    char kind; /* F folder, T type, M master copy */
    int sec;
    char name[256];
    char path[1024]; /* inside the section */
} lib_obj;

/* Resolves "Folder/Sub/Name" in the section(s); want: 'F', 'T', 'M' or 0 (any). */
static int resolve_in(tool_ctx *c, const lib_ref *l, int sec, const char *path_in, char want, lib_obj *out, int quiet)
{
    memset(out, 0, sizeof *out);
    int forced = 0;
    const char *path = strip_section(path_in, &forced);
    if (forced)
        sec = forced;
    for (int s = SEC_TYPES; s <= SEC_MC; s++) {
        if (!(sec & s))
            continue;
        th cur = section_root(l, s);
        char seg[256];
        const char *p = path;
        int ok = cur != 0;
        th found = 0;
        char kind = 'F';
        while (ok && *p) {
            size_t n = strcspn(p, "/\\");
            snprintf(seg, sizeof seg, "%.*s", (int)(n < 255 ? n : 255), p);
            p += n;
            while (*p == '/' || *p == '\\')
                p++;
            int last = !*p;
            th next = child_named(td_get_h(cur, "Folders"), seg);
            if (last && want != 'F' && (!next || want == 'T' || want == 'M')) {
                th item = child_named(td_get_h(cur, s == SEC_MC ? "MasterCopies" : "Types"), seg);
                if (item) {
                    found = item;
                    kind = s == SEC_MC ? 'M' : 'T';
                    break;
                }
            }
            if (!next) {
                ok = 0;
                break;
            }
            cur = next;
            if (last) {
                found = cur;
                kind = 'F';
            }
        }
        if (ok && !*path) {
            found = cur;
            kind = 'F';
        }
        if (found && (!want || want == kind)) {
            out->h = found;
            out->kind = kind;
            out->sec = s;
            snprintf(out->path, sizeof out->path, "%s", path);
            const char *slash = strrchr(path, '/');
            snprintf(out->name, sizeof out->name, "%s", slash ? slash + 1 : path);
            return 0;
        }
    }
    if (!quiet)
        fail(c, "%s '%s' not found in library '%s'%s", want == 'T' ? "type" : want == 'M' ? "master copy" : want == 'F' ? "folder" : "item",
             path, l->name, " (paths are 'Folder/Sub/Name'; see action=get_tree or find)");
    return -1;
}

static int resolve(tool_ctx *c, const lib_ref *l, int sec, const char *path, char want, lib_obj *out)
{
    if (!path || !*path)
        return fail(c, "path is required");
    return resolve_in(c, l, sec, path, want, out, 0);
}

static const char *type_kind(const char *t)
{
    static char buf[64];
    const char *s = t ? strrchr(t, '.') : NULL;
    s = s ? s + 1 : (t ? t : "?");
    snprintf(buf, sizeof buf, "%s", s);
    char *suffix = strstr(buf, "LibraryType");
    if (suffix && suffix != buf)
        *suffix = 0;
    return buf;
}

static char *ml_first_text(th ml)
{
    if (!ml)
        return NULL;
    cJSON *items = td_enum(td_get_h(ml, "Items"), "Text", -1);
    char *res = NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        const char *t = tdi_s(it, "Text");
        if (t && *t) {
            res = _strdup(t);
            break;
        }
    }
    cJSON_Delete(items);
    td_clear_err();
    return res;
}

/* ---- versions ---------------------------------------------------------------------------------- */

static int version_cmp(const char *a, const char *b)
{
    while (a && b && *a && *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y)
            return x < y ? -1 : 1;
        if (*a == '.')
            a++;
        if (*b == '.')
            b++;
    }
    return (a && *a) ? 1 : (b && *b) ? -1 : 0;
}

typedef struct ver_info {
    th h;
    char number[32];
    char state[16];
    int is_default;
} ver_info;

/* Picks a version: "latest_committed" (default), "latest_any", "default" or an explicit number. */
static int pick_version(tool_ctx *c, th type, const char *type_name, const char *wanted, ver_info *out)
{
    memset(out, 0, sizeof *out);
    if (!wanted || !*wanted)
        wanted = "latest_committed";
    cJSON *vs = td_enum(td_get_h(type, "Versions"), "VersionNumber,State,IsDefault", -1);
    strbuf avail;
    sb_init(&avail);
    const cJSON *it;
    cJSON_ArrayForEach(it, vs)
    {
        const char *num = tdi_s(it, "VersionNumber");
        const char *st = tdi_s(it, "State");
        int def = tdi_b(it, "IsDefault", 0);
        sb_printf(&avail, "%s%s (%s%s)", avail.len ? ", " : "", num ? num : "?", st ? st : "?", def ? ", default" : "");
        int take = 0;
        if (_stricmp(wanted, "latest_committed") == 0)
            take = st && strcmp(st, "Committed") == 0 && (!out->h || version_cmp(num, out->number) > 0);
        else if (_stricmp(wanted, "latest_any") == 0 || _stricmp(wanted, "latest") == 0)
            take = !out->h || version_cmp(num, out->number) > 0;
        else if (_stricmp(wanted, "default") == 0)
            take = def;
        else
            take = num && (strcmp(num, wanted) == 0 || (wanted[0] == 'V' && strcmp(num, wanted + 1) == 0));
        if (take) {
            out->h = tdv_h(it);
            snprintf(out->number, sizeof out->number, "%s", num ? num : "?");
            snprintf(out->state, sizeof out->state, "%s", st ? st : "?");
            out->is_default = def;
        }
    }
    cJSON_Delete(vs);
    td_clear_err();
    int rc = 0;
    if (!out->h)
        rc = fail(c, "type '%s' has no version matching '%s'. Versions: %s", type_name, wanted, avail.len ? sb_str(&avail) : "(none)");
    sb_free(&avail);
    return rc;
}

static void type_envelope(th type, const char *type_t, strbuf *sb)
{
    cJSON *vs = td_enum(td_get_h(type, "Versions"), "VersionNumber,State,IsDefault", -1);
    char def[32] = "", latest[32] = "", latest_state[16] = "";
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, vs)
    {
        const char *num = tdi_s(it, "VersionNumber");
        n++;
        if (tdi_b(it, "IsDefault", 0))
            snprintf(def, sizeof def, "%s", num ? num : "?");
        if (!latest[0] || version_cmp(num, latest) > 0) {
            snprintf(latest, sizeof latest, "%s", num ? num : "?");
            snprintf(latest_state, sizeof latest_state, "%s", tdi_s(it, "State") ? tdi_s(it, "State") : "?");
        }
    }
    cJSON_Delete(vs);
    sb_printf(sb, "kind=%s, versions=%d", type_kind(type_t), n);
    if (def[0])
        sb_printf(sb, ", default=%s", def);
    if (latest[0] && strcmp(latest, def) != 0)
        sb_printf(sb, ", latest=%s", latest);
    if (latest_state[0] && strcmp(latest_state, "Committed") != 0)
        sb_printf(sb, " (%s)", latest_state);
    td_clear_err();
}

/* ---- tree walk -------------------------------------------------------------------------------- */

typedef struct walk_ctx walk_ctx;
typedef void (*walk_fn)(walk_ctx *w, char kind, th h, const char *type_t, const char *name, const char *path, int depth);
struct walk_ctx {
    tool_ctx *c;
    int max_depth; /* 0 = unlimited */
    int items;
    int count;
    int limit;
    int truncated;
    walk_fn fn;
    void *ctx;
};

static void walk_folder(walk_ctx *w, int sec, th folder, const char *path, int depth)
{
    if (w->truncated)
        return;
    cJSON *folders = td_enum(td_get_h(folder, "Folders"), "Name", -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, folders)
    {
        if (w->count >= w->limit) {
            w->truncated = 1;
            break;
        }
        const char *n = tdi_s(it, "Name") ? tdi_s(it, "Name") : "?";
        char sub[1024];
        snprintf(sub, sizeof sub, "%s%s%s", path, *path ? "/" : "", n);
        w->count++;
        w->fn(w, 'F', tdv_h(it), NULL, n, sub, depth);
        if (!w->max_depth || depth + 1 < w->max_depth)
            walk_folder(w, sec, tdv_h(it), sub, depth + 1);
    }
    cJSON_Delete(folders);
    if (w->items && !w->truncated) {
        cJSON *items = td_enum(td_get_h(folder, sec == SEC_MC ? "MasterCopies" : "Types"), "Name", -1);
        cJSON_ArrayForEach(it, items)
        {
            if (w->count >= w->limit) {
                w->truncated = 1;
                break;
            }
            const char *n = tdi_s(it, "Name") ? tdi_s(it, "Name") : "?";
            char sub[1024];
            snprintf(sub, sizeof sub, "%s%s%s", path, *path ? "/" : "", n);
            w->count++;
            w->fn(w, sec == SEC_MC ? 'M' : 'T', tdv_h(it), tdv_type(it), n, sub, depth);
        }
        cJSON_Delete(items);
    }
    td_clear_err();
}

typedef struct counts {
    int folders, types, versions, mcs;
} counts;

static void count_fn(walk_ctx *w, char kind, th h, const char *type_t, const char *name, const char *path, int depth)
{
    (void)type_t;
    (void)name;
    (void)path;
    (void)depth;
    counts *k = w->ctx;
    if (kind == 'F')
        k->folders++;
    else if (kind == 'M')
        k->mcs++;
    else {
        k->types++;
        cJSON *vs = td_enum(td_get_h(h, "Versions"), NULL, -1);
        k->versions += cJSON_GetArraySize(vs);
        cJSON_Delete(vs);
    }
}

static void count_library(const lib_ref *l, counts *k)
{
    memset(k, 0, sizeof *k);
    for (int s = SEC_TYPES; s <= SEC_MC; s++) {
        walk_ctx w = { NULL, 0, 1, 0, 100000, 0, count_fn, k };
        walk_folder(&w, s, section_root(l, s), "", 0);
    }
}

/* ---- read actions ------------------------------------------------------------------------------ */

static int a_list_libraries(tool_ctx *c)
{
    if (session_project()) {
        lib_ref p;
        if (find_library_named(c, "project", &p) == 0) {
            counts k;
            count_library(&p, &k);
            out(c, "project  [project library, types=%d, versions=%d, master copies=%d, folders=%d]\n", k.types, k.versions, k.mcs,
                k.folders);
        }
    } else {
        out(c, "(no project open: no project library)\n");
    }
    cJSON *libs = td_enum(td_get_h(session_portal(), "GlobalLibraries"), "Name,Path,IsReadOnly,IsModified", -1);
    const cJSON *it;
    int n = 0;
    cJSON_ArrayForEach(it, libs)
    {
        out(c, "%s  [global, %s%s, path=%s]\n", tdi_s(it, "Name") ? tdi_s(it, "Name") : "?",
            tdi_b(it, "IsReadOnly", 0) ? "ReadOnly" : "ReadWrite", tdi_b(it, "IsModified", 0) ? ", unsaved changes" : "",
            tdi_s(it, "Path") ? tdi_s(it, "Path") : "?");
        n++;
    }
    cJSON_Delete(libs);
    cJSON *infos = td_call(td_get_h(session_portal(), "GlobalLibraries"), "GetGlobalLibraryInfos", NULL);
    cJSON *list = infos && tdv_h(infos) ? td_enum(tdv_h(infos), "Name,Path,IsOpen,LibraryType", -1) : NULL;
    int shown = 0;
    cJSON_ArrayForEach(it, list)
    {
        if (tdi_b(it, "IsOpen", 0))
            continue;
        if (!shown++)
            out(c, "Known but not open:\n");
        out(c, "  %s  [%s, path=%s]\n", tdi_s(it, "Name") ? tdi_s(it, "Name") : "?", tdi_s(it, "LibraryType") ? tdi_s(it, "LibraryType") : "?",
            tdi_s(it, "Path") ? tdi_s(it, "Path") : "?");
    }
    cJSON_Delete(list);
    cJSON_Delete(infos);
    td_clear_err();
    out(c, "%d global librar%s open. Use the name (or 'project') as the library argument.\n", n, n == 1 ? "y" : "ies");
    return 0;
}

static int a_get_info(tool_ctx *c)
{
    lib_ref l;
    if (find_library(c, "library", &l) != 0)
        return -1;
    if (!l.is_project) {
        cJSON *a = td_attrs(l.lib, "Name,Path,Version,Author,Copyright,Family,IsReadOnly,IsModified,IsWriteProtected,LastModified,"
                                   "LastModifiedBy,CreationTime");
        static const char *keys[] = { "Name",       "Path",         "Version",       "Author",         "Copyright", "Family",
                                      "IsReadOnly", "IsModified",   "IsWriteProtected", "LastModified", "LastModifiedBy",
                                      "CreationTime" };
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, keys[i]);
            if (!v || tdv_is_err(v) || cJSON_IsNull(v))
                continue;
            char *t = tdv_text(v);
            if (*t)
                out(c, "%s: %s\n", keys[i], t);
            free(t);
        }
        cJSON_Delete(a);
        char *cm = ml_first_text(td_get_h(l.lib, "Comment"));
        if (cm && *cm)
            out(c, "Comment: %s\n", cm);
        free(cm);
        td_clear_err();
    } else {
        out(c, "Name: project library\n");
    }
    counts k;
    count_library(&l, &k);
    out(c, "Types: %d (%d versions), master copies: %d, folders: %d\n", k.types, k.versions, k.mcs, k.folders);
    for (int s = SEC_TYPES; s <= SEC_MC; s++) {
        cJSON *f = td_enum(td_get_h(section_root(&l, s), "Folders"), "Name", -1);
        strbuf sb;
        sb_init(&sb);
        const cJSON *it;
        cJSON_ArrayForEach(it, f)
        sb_printf(&sb, "%s%s", sb.len ? ", " : "", tdi_s(it, "Name") ? tdi_s(it, "Name") : "?");
        out(c, "%s root folders: %s\n", section_label(s), sb.len ? sb_str(&sb) : "(none)");
        sb_free(&sb);
        cJSON_Delete(f);
    }
    td_clear_err();
    return 0;
}

typedef struct tree_out {
    int nested;
    int sec;
} tree_out;

static void tree_line(walk_ctx *w, char kind, th h, const char *type_t, const char *name, const char *path, int depth)
{
    tree_out *t = w->ctx;
    strbuf env;
    sb_init(&env);
    if (kind == 'T')
        type_envelope(h, type_t, &env);
    else if (kind == 'M') {
        cJSON *cd = td_enum(td_get_h(h, "ContentDescriptions"), "ContentName", -1);
        sb_printf(&env, "master copy, contents=%d", cJSON_GetArraySize(cd));
        cJSON_Delete(cd);
        td_clear_err();
    }
    if (t->nested)
        out(w->c, "%*s%s%s%s%s%s\n", 2 + depth * 2, "", name, kind == 'F' ? "/" : "", env.len ? "  [" : "", sb_str(&env), env.len ? "]" : "");
    else
        out(w->c, "%s/%s%s%s%s%s\n", section_label(t->sec), path, kind == 'F' ? "/" : "", env.len ? "  [" : "", sb_str(&env),
            env.len ? "]" : "");
    sb_free(&env);
}

static int a_get_tree(tool_ctx *c)
{
    lib_ref l;
    if (find_library(c, "library", &l) != 0)
        return -1;
    int sec = parse_section(c, "typeFilter", SEC_ALL);
    const char *path = arg_s(c, "path");
    int nested = arg_b(c, "nested", 1);
    int items = arg_b(c, "includeItems", 0);
    int limit = (int)arg_i(c, "limit", MAX_TREE);
    walk_ctx w = { c, (int)arg_i(c, "depth", 0), items, 0, limit > 0 ? limit : MAX_TREE, 0, tree_line, NULL };
    for (int s = SEC_TYPES; s <= SEC_MC; s++) {
        if (!(sec & s))
            continue;
        th start = section_root(&l, s);
        char base[1024] = "";
        if (path && *path) {
            lib_obj o;
            if (resolve_in(c, &l, s, path, 'F', &o, 1) != 0)
                continue;
            start = o.h;
            snprintf(base, sizeof base, "%s", o.path);
        }
        tree_out t = { nested, s };
        w.ctx = &t;
        if (nested)
            out(c, "%s/%s%s\n", section_label(s), base, *base ? "/" : "");
        walk_folder(&w, s, start, base, nested && *base ? 1 : 0);
    }
    if (path && *path && !w.count)
        out(c, "(folder '%s' not found or empty)\n", path);
    if (w.truncated)
        out(c, "NOTICE: truncated at %d entries - narrow with path or depth, or raise limit.\n", w.limit);
    if (!items)
        out(c, "(folders only: includeItems=true lists types and master copies)\n");
    return 0;
}

typedef struct find_ctx {
    const char *pattern;
    int cs;
    int sec;
    int matches;
} find_ctx;

static void find_line(walk_ctx *w, char kind, th h, const char *type_t, const char *name, const char *path, int depth)
{
    (void)depth;
    find_ctx *f = w->ctx;
    if (!glob_match(f->pattern, name, f->cs))
        return;
    f->matches++;
    strbuf env;
    sb_init(&env);
    if (kind == 'T')
        type_envelope(h, type_t, &env);
    else
        sb_printf(&env, "%s", kind == 'F' ? "folder" : "master copy");
    out(w->c, "%s/%s%s  [%s]\n", section_label(f->sec), path, kind == 'F' ? "/" : "", sb_str(&env));
    sb_free(&env);
}

static int a_find(tool_ctx *c)
{
    lib_ref l;
    if (find_library(c, "library", &l) != 0)
        return -1;
    const char *q = arg_req(c, "query");
    if (!q)
        return -1;
    char pattern[512];
    if (glob_has_wildcards(q))
        snprintf(pattern, sizeof pattern, "%s", q);
    else
        snprintf(pattern, sizeof pattern, "*%s*", q);
    int sec = parse_section(c, "typeFilter", SEC_ALL);
    const char *path = arg_s(c, "path");
    int limit = (int)arg_i(c, "limit", 500);
    find_ctx f = { pattern, arg_b(c, "caseSensitive", 0), 0, 0 };
    walk_ctx w = { c, 0, 1, 0, 100000, 0, find_line, &f };
    for (int s = SEC_TYPES; s <= SEC_MC && f.matches < limit; s++) {
        if (!(sec & s))
            continue;
        th start = section_root(&l, s);
        char base[1024] = "";
        if (path && *path) {
            lib_obj o;
            if (resolve_in(c, &l, s, path, 'F', &o, 1) != 0)
                continue;
            start = o.h;
            snprintf(base, sizeof base, "%s", o.path);
        }
        f.sec = s;
        walk_folder(&w, s, start, base, 0);
    }
    out(c, "%d match(es) for '%s' in library '%s'.\n", f.matches, pattern, l.name);
    return 0;
}

static void version_names(th assoc, strbuf *sb)
{
    cJSON *vs = td_enum(assoc, "VersionNumber,TypeObject", -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, vs)
    {
        char *tn = td_get_s(tdv_h(tdi_a(it, "TypeObject")), "Name");
        sb_printf(sb, "%s%s %s", sb->len ? ", " : "", tn ? tn : "?", tdi_s(it, "VersionNumber") ? tdi_s(it, "VersionNumber") : "?");
        free(tn);
    }
    cJSON_Delete(vs);
    td_clear_err();
}

static int export_version(tool_ctx *c, th version, char *path, size_t cap)
{
    if (fs_temp_path("libtype", ".xml", path, cap) != 0)
        return fail(c, "cannot create a temporary file");
    fs_remove(path);
    if (td_call_v(version, "Export", tda("fe", path, "Siemens.Engineering.ExportOptions", "WithDefaults")) != 0)
        return fail_td(c, "exporting the type version failed");
    return 0;
}

static int a_get_type(tool_ctx *c)
{
    lib_ref l;
    if (find_library(c, "library", &l) != 0)
        return -1;
    lib_obj o;
    if (resolve(c, &l, SEC_TYPES, arg_s(c, "path"), 'T', &o) != 0)
        return -1;
    char *tt = td_typename(o.h);
    cJSON *a = td_attrs(o.h, "Name,Guid,Author,Status,DoNotUse,SetForUpdate,Namespace");
    out(c, "Type %s  [kind=%s, guid=%s, author=%s, status=%s%s%s%s%s]\n", o.path, type_kind(tt),
        tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Guid")) ? tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Guid")) : "?",
        tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Author")) ? tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Author")) : "?",
        tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Status")) ? tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Status")) : "?",
        tdv_b(cJSON_GetObjectItemCaseSensitive(a, "DoNotUse"), 0) ? ", doNotUse" : "",
        tdv_b(cJSON_GetObjectItemCaseSensitive(a, "SetForUpdate"), 0) ? ", setForUpdate" : "",
        tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Namespace")) && *tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Namespace")) ? ", namespace=" : "",
        tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Namespace")) ? tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Namespace")) : "");
    cJSON_Delete(a);
    free(tt);
    char *cm = ml_first_text(td_get_h(o.h, "Comment"));
    if (cm && *cm)
        out(c, "Comment: %s\n", cm);
    free(cm);
    out(c, "Versions:\n");
    cJSON *vs = td_enum(td_get_h(o.h, "Versions"), "VersionNumber,State,IsDefault,Author,ModifiedDate", -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, vs)
    {
        char *when = tdv_text(tdi_a(it, "ModifiedDate"));
        out(c, "  %s  [state=%s%s, author=%s, modified=%s]\n", tdi_s(it, "VersionNumber") ? tdi_s(it, "VersionNumber") : "?",
            tdi_s(it, "State") ? tdi_s(it, "State") : "?", tdi_b(it, "IsDefault", 0) ? ", default" : "",
            tdi_s(it, "Author") ? tdi_s(it, "Author") : "?", when ? when : "?");
        free(when);
    }
    cJSON_Delete(vs);
    ver_info v;
    if (pick_version(c, o.h, o.name, arg_s(c, "version"), &v) != 0)
        return -1;
    strbuf deps, dts;
    sb_init(&deps);
    sb_init(&dts);
    version_names(td_get_h(v.h, "Dependencies"), &deps);
    version_names(td_get_h(v.h, "Dependents"), &dts);
    cJSON *mcs = td_enum(td_get_h(v.h, "MasterCopiesContainingInstances"), "Name", -1);
    out(c, "Version %s: dependencies: %s; dependents: %s; master copies with instances: %d\n", v.number,
        deps.len ? sb_str(&deps) : "none", dts.len ? sb_str(&dts) : "none", cJSON_GetArraySize(mcs));
    cJSON_Delete(mcs);
    sb_free(&deps);
    sb_free(&dts);
    if (l.is_project) { /* instances of the selected version in every PLC of the project */
        cJSON *scope = cJSON_CreateArray();
        nav_each_device(session_project(), collect_plc, scope);
        strbuf inst;
        sb_init(&inst);
        const cJSON *s;
        cJSON_ArrayForEach(s, scope)
        {
            th sw = (th)cJSON_GetObjectItem(s, "$h")->valuedouble;
            cJSON *res = td_call(v.h, "FindInstances", tda("h", sw));
            cJSON *list = tdv_h(res) ? td_enum(tdv_h(res), "LibraryTypeInstance", -1) : NULL;
            char *plc = td_get_s(sw, "Name");
            cJSON_ArrayForEach(it, list)
            {
                char *n = td_get_s(tdv_h(tdi_a(it, "LibraryTypeInstance")), "Name");
                sb_printf(&inst, "%s%s: %s", inst.len ? ", " : "", plc ? plc : "?", n ? n : "?");
                free(n);
            }
            free(plc);
            cJSON_Delete(list);
            cJSON_Delete(res);
        }
        out(c, "Instances of %s in the project: %s\n", v.number, inst.len ? sb_str(&inst) : "none");
        sb_free(&inst);
        cJSON_Delete(scope);
    }
    td_clear_err();
    if (arg_b(c, "includeXml", 0)) {
        char path[TC_PATH_MAX];
        if (export_version(c, v.h, path, sizeof path) != 0)
            return -1;
        char *data = NULL;
        size_t len = 0;
        if (fs_read_all(path, &data, &len) == 0) {
            const char *p = data;
            if (len >= 3 && (unsigned char)p[0] == 0xEF)
                p += 3;
            size_t n = strlen(p);
            out(c, "XML (%zu bytes%s):\n", n, n > 30000 ? ", first 30000 shown - use export_type_xml for the whole file" : "");
            if (n > 30000)
                ((char *)p)[30000] = 0;
            out_raw(c, p);
            out_raw(c, "\n");
            free(data);
        }
        fs_remove(path);
    }
    return 0;
}

static int a_get_master_copy(tool_ctx *c)
{
    lib_ref l;
    if (find_library(c, "library", &l) != 0)
        return -1;
    lib_obj o;
    if (resolve(c, &l, SEC_MC, arg_s(c, "path"), 'M', &o) != 0)
        return -1;
    cJSON *a = td_attrs(o.h, "Name,Author,CreationDate");
    char *when = tdv_text(cJSON_GetObjectItemCaseSensitive(a, "CreationDate"));
    out(c, "Master copy %s  [author=%s, created=%s]\n", o.path,
        tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Author")) ? tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Author")) : "?",
        when ? when : "?");
    free(when);
    cJSON_Delete(a);
    cJSON *cd = td_enum(td_get_h(o.h, "ContentDescriptions"), "ContentName,ContentType", -1);
    out(c, "Contents (%d):\n", cJSON_GetArraySize(cd));
    const cJSON *it;
    cJSON_ArrayForEach(it, cd)
    {
        th type = tdv_h(tdi_a(it, "ContentType"));
        char *tn = type ? td_get_s(type, "Name") : NULL;
        out(c, "  %s  [%s]\n", tdi_s(it, "ContentName") ? tdi_s(it, "ContentName") : "?", tn ? tn : "?");
        free(tn);
    }
    cJSON_Delete(cd);
    td_clear_err();
    out(c, "A master copy carries no dependencies: publish the blocks and types it uses separately. Openness cannot export "
           "the content of a master copy as XML.\n");
    return 0;
}

static int a_export_type_xml(tool_ctx *c)
{
    lib_ref l;
    if (find_library(c, "library", &l) != 0)
        return -1;
    lib_obj o;
    if (resolve(c, &l, SEC_TYPES, arg_s(c, "path"), 'T', &o) != 0)
        return -1;
    ver_info v;
    if (pick_version(c, o.h, o.name, arg_s(c, "version"), &v) != 0)
        return -1;
    const char *outp = arg_s(c, "outputPath");
    if (outp && *outp && fs_is_file(outp) && !arg_b(c, "overwrite", 0))
        return fail(c, "'%s' exists: pass overwrite=true to replace it", outp);
    char path[TC_PATH_MAX], name[400];
    if (export_version(c, v.h, path, sizeof path) != 0)
        return -1;
    snprintf(name, sizeof name, "%s_%s.xml", o.name, v.number);
    out(c, "Type %s version %s exported.\n", o.path, v.number);
    int rc = sw_deliver(c, path, name, "application/xml", outp, arg_b(c, "returnInline", outp && *outp ? 0 : 1));
    fs_remove(path);
    return rc;
}

/* ---- folders, renames, deletes ------------------------------------------------------------------ */

static int a_create_library_folder(tool_ctx *c)
{
    lib_ref l;
    if (find_library(c, "library", &l) != 0 || require_writable(c, &l) != 0)
        return -1;
    int sec = parse_section(c, "typeFilter", SEC_MC);
    if (sec == SEC_ALL)
        sec = SEC_MC;
    const char *fp = arg_req(c, "folderPath");
    if (!fp)
        return -1;
    int forced = 0;
    fp = strip_section(fp, &forced);
    if (forced)
        sec = forced;
    th cur = section_root(&l, sec);
    const char *p = fp;
    int created = 0;
    char seg[256], done[1024] = "";
    while (*p) {
        size_t n = strcspn(p, "/\\");
        snprintf(seg, sizeof seg, "%.*s", (int)(n < 255 ? n : 255), p);
        p += n;
        while (*p == '/' || *p == '\\')
            p++;
        if (!*seg)
            continue;
        th comp = td_get_h(cur, "Folders");
        th next = child_named(comp, seg);
        if (!next) {
            next = td_call_h(comp, "Create", tda("s", seg));
            if (!next)
                return fail_td(c, "creating the folder failed");
            created++;
        }
        size_t dl = strlen(done);
        snprintf(done + dl, sizeof done - dl, "%s%s", dl ? "/" : "", seg);
        cur = next;
    }
    out(c, "%s/%s in library '%s': %s.\n", section_label(sec), done, l.name, created ? "created" : "already exists");
    return 0;
}

static int a_delete_library_folder(tool_ctx *c)
{
    lib_ref l;
    if (find_library(c, "library", &l) != 0 || require_writable(c, &l) != 0)
        return -1;
    int sec = parse_section(c, "typeFilter", SEC_MC);
    if (sec == SEC_ALL)
        sec = SEC_MC;
    lib_obj o;
    if (resolve(c, &l, sec, arg_s(c, "folderPath"), 'F', &o) != 0)
        return -1;
    if (!o.path[0])
        return fail(c, "the root folder of a section cannot be deleted");
    cJSON *f = td_enum(td_get_h(o.h, "Folders"), NULL, 1);
    cJSON *i = td_enum(td_get_h(o.h, o.sec == SEC_MC ? "MasterCopies" : "Types"), NULL, 1);
    int empty = cJSON_GetArraySize(f) == 0 && cJSON_GetArraySize(i) == 0;
    cJSON_Delete(f);
    cJSON_Delete(i);
    if (!empty && !arg_b(c, "recursive", 0))
        return fail(c, "folder '%s' is not empty: pass recursive=true to delete it with its content", o.path);
    if (td_call_v(o.h, "Delete", NULL) != 0)
        return fail_td(c, "deleting the folder failed");
    out(c, "%s/%s deleted from library '%s'.\n", section_label(o.sec), o.path, l.name);
    return 0;
}

static int rename_obj(tool_ctx *c, int sec, char want, const char *path_arg, const char *what)
{
    lib_ref l;
    if (find_library(c, "library", &l) != 0 || require_writable(c, &l) != 0)
        return -1;
    const char *nn = arg_req(c, "newName");
    if (!nn)
        return -1;
    lib_obj o;
    if (resolve(c, &l, sec, arg_s(c, path_arg), want, &o) != 0)
        return -1;
    if (want == 'F' && !o.path[0])
        return fail(c, "the root folder of a section cannot be renamed");
    if (td_set(o.h, "Name", cJSON_CreateString(nn)) != 0)
        return fail_td(c, "renaming failed");
    out(c, "%s '%s' renamed to '%s'.\n", what, o.path, nn);
    return 0;
}

static int a_rename_library_folder(tool_ctx *c)
{
    int sec = parse_section(c, "typeFilter", SEC_MC);
    return rename_obj(c, sec == SEC_ALL ? SEC_MC : sec, 'F', "folderPath", "Folder");
}

static int a_rename_master_copy(tool_ctx *c)
{
    int rc = rename_obj(c, SEC_MC, 'M', "path", "Master copy");
    if (rc == 0)
        out(c, "The objects inside the master copy keep their own names.\n");
    return rc;
}

static int a_rename_type(tool_ctx *c)
{
    int rc = rename_obj(c, SEC_TYPES, 'T', "path", "Type");
    if (rc == 0)
        out(c, "Versions and blocks already instantiated in PLCs are unchanged.\n");
    return rc;
}

static int a_delete_master_copy(tool_ctx *c)
{
    lib_ref l;
    if (find_library(c, "library", &l) != 0 || require_writable(c, &l) != 0)
        return -1;
    lib_obj o;
    if (resolve(c, &l, SEC_MC, arg_s(c, "path"), 'M', &o) != 0)
        return -1;
    if (td_call_v(o.h, "Delete", NULL) != 0)
        return fail_td(c, "deleting the master copy failed");
    out(c, "Master copy '%s' deleted from library '%s'.\n", o.path, l.name);
    return 0;
}

/* ---- publish / instantiate --------------------------------------------------------------------------- */

static int publish(tool_ctx *c, sw_container cont, const char *name_arg, const char *what)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *name = arg_req(c, name_arg);
    if (!name)
        return -1;
    sw_found src;
    if (sw_find(c, plc.software, cont, name, &src) != 0)
        return -1;
    lib_ref l;
    if (find_library(c, "library", &l) != 0 || require_writable(c, &l) != 0)
        return -1;
    th folder = section_root(&l, SEC_MC);
    const char *fp = arg_s(c, "folderPath");
    if (fp && *fp) {
        lib_obj o;
        if (resolve(c, &l, SEC_MC, fp, 'F', &o) != 0)
            return -1;
        folder = o.h;
    }
    th mcs = td_get_h(folder, "MasterCopies");
    const char *on = arg_s(c, "onCollision");
    if (!on || !*on)
        on = "autoRename";
    th existing = child_named(mcs, src.name);
    if (existing && _stricmp(on, "fail") == 0)
        return fail(c, "a master copy named '%s' already exists: pass onCollision=replace or autoRename", src.name);
    if (existing && _stricmp(on, "replace") == 0 && td_call_v(existing, "Delete", NULL) != 0)
        return fail_td(c, "replacing the existing master copy failed");
    th mc = td_call_h(mcs, "Create", tda("h", src.item));
    if (!mc)
        return fail_td(c, "creating the master copy failed");
    char *mn = td_get_s(mc, "Name");
    out(c, "%s '%s' of %s published as master copy '%s%s%s' in library '%s'%s.\n", what, src.name, plc.device_name,
        fp && *fp ? fp : "", fp && *fp ? "/" : "", mn ? mn : "?", l.name,
        existing && _stricmp(on, "replace") == 0 ? " (replaced the existing one)" : "");
    free(mn);
    out(c, "Dependencies are not included: publish the blocks and types it uses separately.\n");
    return 0;
}

static int a_publish_block_as_master_copy(tool_ctx *c)
{
    return publish(c, SWC_BLOCKS, "blockName", "Block");
}

static int a_publish_plc_type_as_master_copy(tool_ctx *c)
{
    return publish(c, SWC_TYPES, "udtName", "PLC data type");
}

static int a_instantiate_master_copy(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    lib_ref l;
    if (find_library(c, "library", &l) != 0)
        return -1;
    lib_obj o;
    if (resolve(c, &l, SEC_MC, arg_s(c, "path"), 'M', &o) != 0)
        return -1;
    /* the target composition follows the content type of the master copy */
    cJSON *cd = td_enum(td_get_h(o.h, "ContentDescriptions"), "ContentType", -1);
    char kind[128] = "";
    if (cJSON_GetArraySize(cd)) {
        char *tn = td_get_s(tdv_h(tdi_a(cJSON_GetArrayItem(cd, 0), "ContentType")), "FullName");
        snprintf(kind, sizeof kind, "%s", tn ? tn : "");
        free(tn);
    }
    cJSON_Delete(cd);
    td_clear_err();
    th target;
    const char *where;
    if (strstr(kind, ".Types.") || strstr(kind, "PlcStruct") || strstr(kind, "PlcType")) {
        target = td_get_h(td_get_h(plc.software, "TypeGroup"), "Types");
        where = "PLC data types";
    } else if (strstr(kind, ".Tags.PlcTagTable") && !strstr(kind, "Group")) {
        target = td_get_h(td_get_h(plc.software, "TagTableGroup"), "TagTables");
        where = "tag tables";
    } else if (strstr(kind, "BlockUserGroup") || strstr(kind, "BlockGroup")) {
        target = td_get_h(td_get_h(plc.software, "BlockGroup"), "Groups");
        where = "program block folders";
    } else {
        target = td_get_h(td_get_h(plc.software, "BlockGroup"), "Blocks");
        where = "program blocks";
    }
    cJSON *res = target ? td_call(target, "CreateFrom", tda("h", o.h)) : NULL;
    if (!res)
        return fail_td(c, "instantiating the master copy failed");
    strbuf names;
    sb_init(&names);
    th rh = tdv_h(res);
    if (rh && td_is(rh, "System.Collections.IEnumerable")) {
        cJSON *items = td_enum(rh, "Name", -1);
        const cJSON *it;
        cJSON_ArrayForEach(it, items)
        sb_printf(&names, "%s%s", names.len ? ", " : "", tdi_s(it, "Name") ? tdi_s(it, "Name") : "?");
        cJSON_Delete(items);
    } else if (rh) {
        char *n = td_get_s(rh, "Name");
        sb_append(&names, n ? n : "?");
        free(n);
    }
    cJSON_Delete(res);
    td_clear_err();
    nav_cache_clear();
    out(c, "Master copy '%s' instantiated in %s of %s: %s.\n", o.path, where, plc.device_name, names.len ? sb_str(&names) : "(done)");
    sb_free(&names);
    return 0;
}

static int instantiate_type(tool_ctx *c, nav_plc *plc, lib_obj *o, ver_info *v, char *created, size_t cap)
{
    char *tt = td_typename(o->h);
    int is_udt = tt && strstr(tt, "PlcTypeLibraryType") != NULL;
    int is_doc = tt && strstr(tt, "PlcDocument") != NULL;
    free(tt);
    if (strcmp(v->state, "Committed") != 0)
        return fail(c, "version %s of '%s' is %s: only committed versions can be instantiated", v->number, o->name, v->state);
    th target = is_udt ? td_get_h(td_get_h(plc->software, "TypeGroup"), "Types")
                       : is_doc ? 0 : td_get_h(td_get_h(plc->software, "BlockGroup"), "Blocks");
    if (!target)
        return fail(c, "type '%s' (%s) cannot be instantiated in a PLC by this server", o->name, is_doc ? "PLC document" : "?");
    th obj = td_call_h(target, "CreateFrom", tda("h", v->h));
    if (!obj)
        return fail_td(c, "instantiating the type failed");
    char *n = td_get_s(obj, "Name");
    snprintf(created, cap, "%s", n ? n : o->name);
    free(n);
    nav_cache_clear();
    return 0;
}

static int a_instantiate_library_type(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    lib_ref l;
    if (find_library(c, "library", &l) != 0)
        return -1;
    lib_obj o;
    if (resolve(c, &l, SEC_TYPES, arg_s(c, "path"), 'T', &o) != 0)
        return -1;
    ver_info v;
    if (pick_version(c, o.h, o.name, arg_s(c, "version"), &v) != 0)
        return -1;
    char created[256];
    if (instantiate_type(c, &plc, &o, &v, created, sizeof created) != 0)
        return -1;
    out(c, "Type '%s' version %s instantiated in %s as '%s'%s.\n", o.path, v.number, plc.device_name, created,
        l.is_project ? "" : " (the type and its dependencies are now also in the project library)");
    return 0;
}

static int a_add_to_multi_instance_fb(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    lib_ref l;
    if (find_library(c, "library", &l) != 0)
        return -1;
    lib_obj o;
    if (resolve(c, &l, SEC_TYPES, arg_s(c, "path"), 'T', &o) != 0)
        return -1;
    /* the type must already have an FB instance in this PLC (instantiate_library_type) */
    sw_found blk;
    if (sw_find(c, plc.software, SWC_BLOCKS, o.name, &blk) != 0) {
        c->is_error = 0;
        sb_clear(&c->out);
        return fail(c, "type '%s' has no block in %s yet: run action=instantiate_library_type first", o.name, plc.device_name);
    }
    if (strcmp(blk.type, "FB") != 0)
        return fail(c, "'%s' is a %s: multi-instances need an FB type", blk.name, blk.type);
    return bw_add_multi_instance(c, blk.name);
}

/* ---- global library lifecycle --------------------------------------------------------------------------- */

typedef struct lib_file_ctx {
    tool_ctx *c;
    int n;
} lib_file_ctx;

static int list_lib_file(void *ctx, const char *path, int is_dir, long long size, long long mtime)
{
    (void)size;
    (void)mtime;
    lib_file_ctx *k = ctx;
    if (is_dir)
        return 0;
    const char *dot = strrchr(path, '.');
    if (dot && (_strnicmp(dot, ".al", 3) == 0)) {
        out(k->c, "  %s\n", path);
        k->n++;
    }
    return 0;
}

static int a_open_global_library(tool_ctx *c)
{
    const char *path = arg_s(c, "path");
    if (!path || !*path) {
        if (!g_cfg.libraries_root[0])
            return fail(c, "pass path (.al21 file), or set librariesRoot with session action=configure");
        out(c, "Global libraries under %s:\n", g_cfg.libraries_root);
        lib_file_ctx k = { c, 0 };
        fs_walk(g_cfg.libraries_root, NULL, 3, list_lib_file, &k);
        if (!k.n)
            out(c, "  (none)\n");
        return 0;
    }
    char full[TC_PATH_MAX];
    if (fs_full_path(path, full, sizeof full) != 0)
        return fail(c, "invalid path '%s'", path);
    if (fs_is_dir(full)) { /* a library folder: take the .al* file inside */
        char probe[TC_PATH_MAX];
        snprintf(probe, sizeof probe, "%s\\%s.al21", full, fs_basename(full));
        if (fs_is_file(probe))
            snprintf(full, sizeof full, "%s", probe);
    }
    if (!fs_is_file(full))
        return fail(c, "library file '%s' not found", full);
    lib_ref existing;
    if (lookup_global(full, &existing) == 0) {
        out(c, "Library '%s' is already open (%s).\n", existing.name, existing.read_only ? "ReadOnly" : "ReadWrite");
        return 0;
    }
    const char *mode = arg_s(c, "openMode");
    int rw = mode && _stricmp(mode, "ReadWrite") == 0;
    th gl = td_get_h(session_portal(), "GlobalLibraries");
    progress(c, 0, 0, "opening the global library");
    th lib = arg_b(c, "upgrade", 0) ? td_call_h(gl, "OpenWithUpgrade", tda("f", full))
                                    : td_call_h(gl, "Open", tda("fe", full, "Siemens.Engineering.OpenMode", rw ? "ReadWrite" : "ReadOnly"));
    if (!lib) {
        const char *e = td_err();
        if (strstr(e, "upgrade") || strstr(e, "Upgrade") || strstr(e, "version"))
            return fail(c, "opening %s failed: %s. A library from an older TIA Portal version needs upgrade=true (TIA creates an "
                           "upgraded copy).",
                        full, e);
        return fail_td(c, "opening the global library failed");
    }
    char *n = td_get_s(lib, "Name");
    int ro = 1;
    td_get_b(lib, "IsReadOnly", &ro);
    out(c, "Global library '%s' opened (%s). Use library='%s' in the other actions.\n", n ? n : "?", ro ? "ReadOnly" : "ReadWrite",
        n ? n : "?");
    free(n);
    return 0;
}

static int global_only(tool_ctx *c, lib_ref *l, const char *arg)
{
    const char *name = arg_req(c, arg);
    if (!name)
        return -1;
    if (_stricmp(name, "project") == 0)
        return fail(c, "this action applies to global libraries, not to the project library");
    if (find_library_named(c, name, l) != 0)
        return -1;
    if (!td_is(l->lib, T_USER_GLOBAL))
        return fail(c, "'%s' is a system or corporate library: it cannot be saved, closed or archived here", l->name);
    return 0;
}

static int a_close_global_library(tool_ctx *c)
{
    lib_ref l;
    if (global_only(c, &l, "libraryName") != 0)
        return -1;
    int modified = 0;
    td_get_b(l.lib, "IsModified", &modified);
    if (modified && !arg_b(c, "discardChanges", 0))
        return fail(c, "library '%s' has unsaved changes: save_global_library first, or pass discardChanges=true", l.name);
    if (td_call_v(l.lib, "Close", NULL) != 0)
        return fail_td(c, "closing the library failed");
    out(c, "Global library '%s' closed%s.\n", l.name, modified ? " (unsaved changes discarded)" : "");
    return 0;
}

static int a_save_global_library(tool_ctx *c)
{
    lib_ref l;
    if (global_only(c, &l, "libraryName") != 0 || require_writable(c, &l) != 0)
        return -1;
    if (td_call_v(l.lib, "Save", NULL) != 0)
        return fail_td(c, "saving the library failed");
    out(c, "Global library '%s' saved (%s).\n", l.name, l.path);
    return 0;
}

static int a_create_global_library(tool_ctx *c)
{
    const char *name = arg_req(c, "name");
    if (!name)
        return -1;
    const char *dir = arg_s(c, "targetDirectory");
    if (!dir || !*dir)
        dir = g_cfg.libraries_root;
    if (!*dir)
        return fail(c, "pass targetDirectory or set librariesRoot with session action=configure");
    char full[TC_PATH_MAX], target[TC_PATH_MAX];
    if (fs_full_path(dir, full, sizeof full) != 0 || fs_mkdirs(full) != 0)
        return fail(c, "cannot use directory '%s'", dir);
    fs_join(target, sizeof target, full, name);
    if (fs_is_dir(target))
        return fail(c, "'%s' already exists", target);
    th gl = td_get_h(session_portal(), "GlobalLibraries");
    cJSON *res = td_call_generic(gl, "Create", T_USER_GLOBAL, tda("Ds", full, name));
    th lib = tdv_h(res);
    cJSON_Delete(res);
    if (!lib)
        return fail_td(c, "creating the global library failed");
    char *p = td_get_s(lib, "Path");
    out(c, "Global library '%s' created at %s and open read-write. Call save_global_library to persist changes.\n", name, p ? p : target);
    free(p);
    return 0;
}

static int a_archive_global_library(tool_ctx *c)
{
    lib_ref l;
    if (global_only(c, &l, "libraryName") != 0)
        return -1;
    const char *dir = arg_s(c, "targetDirectory");
    if (!dir || !*dir)
        dir = g_cfg.archives_root;
    if (!*dir)
        return fail(c, "pass targetDirectory or set archivesRoot with session action=configure");
    char full[TC_PATH_MAX];
    if (fs_full_path(dir, full, sizeof full) != 0 || fs_mkdirs(full) != 0)
        return fail(c, "cannot use directory '%s'", dir);
    const char *tn = arg_s(c, "targetName");
    char name[300];
    snprintf(name, sizeof name, "%s", tn && *tn ? tn : l.name);
    const char *mode = arg_s(c, "mode");
    if (!mode || !*mode)
        mode = "Compressed";
    /* TIA uses targetName as the file name as given: add the extension of a compressed archive */
    if (strstr(mode, "Compressed") && _stricmp(name + (strlen(name) > 6 ? strlen(name) - 6 : 0), ".zal21") != 0)
        strncat(name, ".zal21", sizeof name - strlen(name) - 1);
    progress(c, 0, 0, "archiving the library");
    if (td_call_v(l.lib, "Archive", tda("Dse", full, name, "Siemens.Engineering.Library.LibraryArchivationMode", mode)) != 0)
        return fail_td(c, "archiving the library failed");
    char path[TC_PATH_MAX];
    fs_join(path, sizeof path, full, name);
    out(c, "Global library '%s' archived: %s (mode %s).\n", l.name, path, mode);
    return 0;
}

static int a_reload_global_library(tool_ctx *c)
{
    lib_ref l;
    if (global_only(c, &l, "libraryName") != 0)
        return -1;
    int modified = 0;
    td_get_b(l.lib, "IsModified", &modified);
    if (modified && !arg_b(c, "discardChanges", 0))
        return fail(c, "library '%s' has unsaved changes that a reload would discard: save it first, or pass discardChanges=true",
                    l.name);
    char path[1024];
    snprintf(path, sizeof path, "%s", l.path);
    int ro = l.read_only;
    if (td_call_v(l.lib, "Close", NULL) != 0)
        return fail_td(c, "closing the library failed");
    th gl = td_get_h(session_portal(), "GlobalLibraries");
    th lib = td_call_h(gl, "Open", tda("fe", path, "Siemens.Engineering.OpenMode", ro ? "ReadOnly" : "ReadWrite"));
    if (!lib)
        return fail_td(c, "re-opening the library failed (it is now closed)");
    out(c, "Global library '%s' reloaded from %s (%s).\n", l.name, path, ro ? "ReadOnly" : "ReadWrite");
    return 0;
}

/* ---- update / promote / compare / clean up --------------------------------------------------------- */

/* Types or folders selected by path (whole library when empty). */
static cJSON *selection(tool_ctx *c, lib_ref *l, const char *path)
{
    cJSON *arr = cJSON_CreateArray();
    if (!path || !*path) {
        cJSON *h = cJSON_CreateObject();
        cJSON_AddNumberToObject(h, "$h", (double)section_root(l, SEC_TYPES));
        cJSON_AddItemToArray(arr, h);
        return arr;
    }
    lib_obj o;
    if (resolve(c, l, SEC_TYPES, path, 0, &o) != 0) {
        cJSON_Delete(arr);
        return NULL;
    }
    cJSON *h = cJSON_CreateObject();
    cJSON_AddNumberToObject(h, "$h", (double)o.h);
    cJSON_AddItemToArray(arr, h);
    return arr;
}

/* All PLC software of the project (update / search scope). */
static int collect_plc(void *ctx, th device, const cJSON *item, const char *group)
{
    (void)item;
    (void)group;
    th sw = nav_device_plc(device, NULL);
    if (sw) {
        cJSON *h = cJSON_CreateObject();
        cJSON_AddNumberToObject(h, "$h", (double)sw);
        cJSON_AddItemToArray((cJSON *)ctx, h);
    }
    return 0;
}

static void print_update_messages(tool_ctx *c, th messages, int depth, int *lines)
{
    if (!messages || depth > 10 || *lines > 2000)
        return;
    cJSON *list = td_enum(messages, "Description", -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        th m = tdv_h(it);
        strbuf parts;
        sb_init(&parts);
        cJSON *kv = td_enum(td_get_h(m, "MessageParts"), "Key,Value", -1);
        const cJSON *p;
        cJSON_ArrayForEach(p, kv)
        sb_printf(&parts, "%s%s=%s", parts.len ? ", " : "", tdi_s(p, "Key") ? tdi_s(p, "Key") : "?",
                  tdi_s(p, "Value") ? tdi_s(p, "Value") : "");
        cJSON_Delete(kv);
        out(c, "%*s%s%s%s%s\n", depth * 2, "", tdi_s(it, "Description") ? tdi_s(it, "Description") : "", parts.len ? " [" : "",
            sb_str(&parts), parts.len ? "]" : "");
        sb_free(&parts);
        (*lines)++;
        print_update_messages(c, td_get_h(m, "Messages"), depth + 1, lines);
    }
    cJSON_Delete(list);
    td_clear_err();
}

static int a_update_check(tool_ctx *c)
{
    lib_ref l;
    if (find_library(c, "library", &l) != 0)
        return -1;
    const char *mode = arg_s(c, "mode");
    progress(c, 0, 0, "checking library updates");
    th res = td_call_h(l.lib, "UpdateCheck",
                       tda("he", session_project(), "Siemens.Engineering.Library.Types.UpdateCheckMode", mode && *mode ? mode : "ReportOutOfDateOnly"));
    if (!res)
        return fail_td(c, "update check failed");
    int lines = 0;
    print_update_messages(c, td_get_h(res, "Messages"), 0, &lines);
    if (!lines)
        out(c, "(no messages)\n");
    const char *path = arg_s(c, "path");
    if (path && *path)
        out(c, "Note: the check covers the whole library; path only matters for update_project.\n");
    out(c, "Read-only check of library '%s' against the project. update_project applies the updates.\n", l.name);
    return 0;
}

/* Snapshot of the program blocks and PLC data types of every PLC ("PLC: kind Name" lines),
   to report what an update removed: TIA deletes the type instances nothing uses. */
typedef struct snap_ctx {
    strbuf *out;
    const char *plc;
    const char *kind;
} snap_ctx;

static int snap_item(void *ctx, const cJSON *item, const char *folder)
{
    snap_ctx *k = ctx;
    sb_printf(k->out, "%s: %s %s%s%s\n", k->plc, k->kind, folder && *folder ? folder : "", folder && *folder ? "/" : "",
              tdi_s(item, "Name") ? tdi_s(item, "Name") : "?");
    return 0;
}

static int snap_device(void *ctx, th device, const cJSON *item, const char *group)
{
    (void)item;
    (void)group;
    th sw = nav_device_plc(device, NULL);
    if (!sw)
        return 0;
    char *name = td_get_s(sw, "Name");
    snap_ctx k = { ctx, name ? name : "?", "block" };
    sw_walk(sw, SWC_BLOCKS, "Name", snap_item, NULL, &k);
    k.kind = "type";
    sw_walk(sw, SWC_TYPES, "Name", snap_item, NULL, &k);
    free(name);
    td_clear_err();
    return 0;
}

static int a_update_project(tool_ctx *c)
{
    lib_ref l;
    if (find_library(c, "library", &l) != 0)
        return -1;
    cJSON *sel = selection(c, &l, arg_s(c, "path"));
    if (!sel)
        return -1;
    cJSON *scope = cJSON_CreateArray();
    nav_each_device(session_project(), collect_plc, scope);
    if (!cJSON_GetArraySize(scope)) {
        cJSON_Delete(sel);
        cJSON_Delete(scope);
        return fail(c, "the project has no PLC to update");
    }
    int nplc = cJSON_GetArraySize(scope);
    strbuf before, after;
    sb_init(&before);
    sb_init(&after);
    nav_each_device(session_project(), snap_device, &before);
    const char *del = arg_b(c, "deleteUnusedVersions", 0) ? "AutomaticallyDelete" : "DoNotDelete";
    progress(c, 0, 0, "updating the project");
    cJSON *args = l.is_project ? tda("jje", sel, scope, "Siemens.Engineering.Library.Types.DeleteUnusedVersionsMode", del)
                               : tda("jjeee", sel, scope, "Siemens.Engineering.Library.Types.ForceUpdateMode",
                                     "SetOnlyHigherUpdatedVersionAsDefault", "Siemens.Engineering.Library.Types.DeleteUnusedVersionsMode",
                                     del, "Siemens.Engineering.Library.Types.StructureConflictResolutionMode",
                                     "CancelIfStructureConflicts");
    if (td_call_v(l.lib, "UpdateProject", args) != 0) {
        sb_free(&before);
        sb_free(&after);
        return fail_td(c, "updating the project failed");
    }
    nav_cache_clear();
    nav_each_device(session_project(), snap_device, &after);
    out(c, "Project updated from library '%s' (%s, %d PLC(s)%s). Compile the PLCs to check the result.\n", l.name,
        arg_s(c, "path") && *arg_s(c, "path") ? arg_s(c, "path") : "all types", nplc,
        arg_b(c, "deleteUnusedVersions", 0) ? ", unused versions deleted" : "");
    /* lines of before missing from after */
    int removed = 0;
    for (const char *p = sb_str(&before); *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char line[600];
        snprintf(line, sizeof line, "%.*s\n", (int)(n < 590 ? n : 590), p);
        if (!strstr(sb_str(&after), line)) {
            if (!removed++)
                out(c, "TIA Portal removed these objects (type instances that nothing used):\n");
            out(c, "  %s", line);
        }
        p = nl ? nl + 1 : p + n;
    }
    if (removed)
        out(c, "Undo the call in TIA Portal (one step) or instantiate the types again if they are needed.\n");
    sb_free(&before);
    sb_free(&after);
    return 0;
}

static int a_promote_to_global(tool_ctx *c)
{
    lib_ref src, dst;
    if (find_library(c, "sourceLibrary", &src) != 0)
        return -1;
    const char *tn = arg_req(c, "targetLibraryName");
    if (!tn || find_library_named(c, tn, &dst) != 0)
        return -1;
    if (dst.is_project)
        return fail(c, "the target must be a global library");
    if (require_writable(c, &dst) != 0)
        return -1;
    cJSON *sel = selection(c, &src, arg_s(c, "path"));
    if (!sel)
        return -1;
    const char *force = arg_s(c, "forceUpdateMode");
    const char *conflict = arg_s(c, "structureConflictMode");
    char force_e[64], conflict_e[64];
    snprintf(force_e, sizeof force_e, "%s",
             !force || !*force || _stricmp(force, "SetOnlyHigher") == 0 ? "SetOnlyHigherUpdatedVersionAsDefault"
             : _stricmp(force, "ForceSetAny") == 0                       ? "ForceSetAnyUpdatedVersionAsDefault"
                                                                         : force);
    snprintf(conflict_e, sizeof conflict_e, "%s",
             !conflict || !*conflict || _stricmp(conflict, "CancelIfConflicts") == 0 ? "CancelIfStructureConflicts" : conflict);
    progress(c, 0, 0, "updating the global library");
    if (td_call_v(src.lib, "UpdateLibrary",
                  tda("jheee", sel, dst.lib, "Siemens.Engineering.Library.Types.ForceUpdateMode", force_e,
                      "Siemens.Engineering.Library.Types.DeleteUnusedVersionsMode",
                      arg_b(c, "deleteUnusedVersions", 0) ? "AutomaticallyDelete" : "DoNotDelete",
                      "Siemens.Engineering.Library.Types.StructureConflictResolutionMode", conflict_e)) != 0)
        return fail_td(c, "updating the global library failed");
    out(c, "Types of library '%s' (%s) copied to '%s'. Save it with save_global_library.\n", src.name,
        arg_s(c, "path") && *arg_s(c, "path") ? arg_s(c, "path") : "all", dst.name);
    return 0;
}

typedef struct cmp_ctx {
    tool_ctx *c;
    int include_identical;
    int only_src, only_dst, differs, identical, lines;
} cmp_ctx;

static void compare_walk(cmp_ctx *k, th element, int depth)
{
    if (!element || depth > 12 || k->lines > 2000)
        return;
    cJSON *a = td_attrs(element, "LeftName,RightName,ComparisonResult,DetailedInformation");
    const char *res = tdv_s(cJSON_GetObjectItemCaseSensitive(a, "ComparisonResult"));
    const char *left = tdv_s(cJSON_GetObjectItemCaseSensitive(a, "LeftName"));
    const char *right = tdv_s(cJSON_GetObjectItemCaseSensitive(a, "RightName"));
    const char *info = tdv_s(cJSON_GetObjectItemCaseSensitive(a, "DetailedInformation"));
    int identical = res && (strcmp(res, "ObjectsIdentical") == 0 || strcmp(res, "ContainerContentsIdentical") == 0);
    const char *verdict = !res ? "?" : strcmp(res, "RightMissing") == 0 ? "only_in_source"
                                     : strcmp(res, "LeftMissing") == 0  ? "only_in_target"
                                     : identical                        ? "identical"
                                                                        : "differs";
    cJSON *kids = td_enum(td_get_h(element, "Elements"), NULL, -1);
    int leaf = cJSON_GetArraySize(kids) == 0;
    if (leaf && depth > 0) {
        if (strcmp(verdict, "only_in_source") == 0)
            k->only_src++;
        else if (strcmp(verdict, "only_in_target") == 0)
            k->only_dst++;
        else if (identical)
            k->identical++;
        else
            k->differs++;
    }
    if (depth == 0 || k->include_identical || !identical) {
        out(k->c, "%*s%s  [%s%s%s]\n", depth * 2, "", left && *left ? left : (right ? right : "?"), verdict,
            info && *info && !identical ? ": " : "", info && *info && !identical ? info : "");
        k->lines++;
    }
    cJSON_Delete(a);
    if (!identical || k->include_identical) {
        const cJSON *e;
        cJSON_ArrayForEach(e, kids)
        compare_walk(k, tdv_h(e), depth + 1);
    }
    cJSON_Delete(kids);
}

static int a_compare_to_target(tool_ctx *c)
{
    lib_ref src, dst;
    if (find_library(c, "library", &src) != 0)
        return -1;
    const char *tn = arg_req(c, "targetLibrary");
    if (!tn || find_library_named(c, tn, &dst) != 0)
        return -1;
    progress(c, 0, 0, "comparing libraries");
    th res = td_call_h(src.lib, "CompareToLibrary", tda("h", dst.lib));
    if (!res)
        return fail_td(c, "comparing the libraries failed");
    cmp_ctx k = { c, arg_b(c, "includeIdentical", 0), 0, 0, 0, 0, 0 };
    compare_walk(&k, td_get_h(res, "RootElement"), 0);
    out(c, "Summary (%s -> %s): only_in_source=%d, only_in_target=%d, differs=%d, identical=%d.\n", src.name, dst.name, k.only_src,
        k.only_dst, k.differs, k.identical);
    return 0;
}

typedef struct unused_ctx {
    tool_ctx *c;
    cJSON *scope;
    int is_project;
    int types, unused;
} unused_ctx;

static void unused_fn(walk_ctx *w, char kind, th h, const char *type_t, const char *name, const char *path, int depth)
{
    (void)type_t;
    (void)name;
    (void)depth;
    if (kind != 'T')
        return;
    unused_ctx *k = w->ctx;
    k->types++;
    cJSON *vs = td_enum(td_get_h(h, "Versions"), "VersionNumber,IsDefault", -1);
    int used = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, vs)
    {
        th v = tdv_h(it);
        cJSON *dts = td_enum(td_get_h(v, "Dependents"), NULL, 1);
        used |= cJSON_GetArraySize(dts) > 0;
        cJSON_Delete(dts);
        if (k->is_project) {
            const cJSON *s;
            cJSON_ArrayForEach(s, k->scope)
            {
                cJSON *inst = td_call(v, "FindInstances", tda("h", (th)cJSON_GetObjectItem(s, "$h")->valuedouble));
                th ih = tdv_h(inst);
                cJSON *list = ih ? td_enum(ih, NULL, 1) : NULL;
                used |= cJSON_GetArraySize(list) > 0;
                cJSON_Delete(list);
                cJSON_Delete(inst);
            }
        }
    }
    cJSON_Delete(vs);
    td_clear_err();
    if (!used) {
        k->unused++;
        out(k->c, "  %s  [no instances%s]\n", path, k->is_project ? " in the project PLCs" : " or dependents in the library");
    }
}

static int a_delete_unused_types(tool_ctx *c)
{
    lib_ref l;
    if (find_library(c, "library", &l) != 0)
        return -1;
    const char *path = arg_s(c, "path");
    th start = section_root(&l, SEC_TYPES);
    char base[1024] = "";
    if (path && *path) {
        lib_obj o;
        if (resolve(c, &l, SEC_TYPES, path, 'F', &o) != 0)
            return -1;
        start = o.h;
        snprintf(base, sizeof base, "%s", o.path);
    }
    unused_ctx k = { c, cJSON_CreateArray(), l.is_project, 0, 0 };
    if (l.is_project)
        nav_each_device(session_project(), collect_plc, k.scope);
    out(c, "Types without instances in library '%s':\n", l.name);
    walk_ctx w = { c, 0, 1, 0, 100000, 0, unused_fn, &k };
    walk_folder(&w, SEC_TYPES, start, base, 0);
    cJSON_Delete(k.scope);
    if (!k.unused)
        out(c, "  (none)\n");
    const char *mode = arg_s(c, "mode");
    int delete_all = mode && _stricmp(mode, "delete_all") == 0;
    if (arg_b(c, "dryRun", 1)) {
        out(c, "%d of %d type(s) unused. Dry run: nothing deleted. Repeat with dryRun=false and confirm='yes' to clean up "
               "(mode=%s).\n",
            k.unused, k.types, delete_all ? "delete_all" : "preserve_default");
        return 0;
    }
    if (require_writable(c, &l) != 0)
        return -1;
    if (!confirmed(c, "yes") && !confirmed(c, "I understand"))
        return fail(c, "deleting library types needs confirm='yes'");
    cJSON *sel = selection(c, &l, path);
    if (!sel)
        return -1;
    counts before;
    count_library(&l, &before);
    int rc = l.is_project ? td_call_v(l.lib, "CleanUpLibrary",
                                      tda("je", sel, "Siemens.Engineering.Library.Types.CleanUpMode",
                                          delete_all ? "DeleteUnusedTypes" : "PreserveDefaultVersionOfUnusedTypes"))
                          : td_call_v(l.lib, "CleanUpLibrary", tda("j", sel));
    if (rc != 0)
        return fail_td(c, "cleaning up the library failed");
    counts after;
    count_library(&l, &after);
    out(c, "Library '%s' cleaned up by TIA Portal (%s): types %d -> %d, versions %d -> %d.\n", l.name,
        l.is_project ? (delete_all ? "mode delete_all" : "mode preserve_default") : "global library clean-up", before.types,
        after.types, before.versions, after.versions);
    if (after.types == before.types && after.versions == before.versions)
        out(c, "Nothing was removed: TIA keeps types that it still considers in use (e.g. referenced by other types or "
               "master copies).\n");
    return 0;
}

/* ---- tool ------------------------------------------------------------------------------------------------- */

#define P AF_PROJECT
#define W (AF_PROJECT | AF_WRITES)
#define G AF_PORTAL
#define GW (AF_PORTAL | AF_WRITES)

static const action_def actions[] = {
    { "add_to_multi_instance_fb", "deviceName, parentFB, memberName, path; optional library='project', onCollision=fail|replace",
      "Add a library FB type as a static multi-instance member of parentFB. The type must already have its block in the PLC "
      "(instantiate_library_type first); then this is blocks_write add_multi_instance_member.",
      a_add_to_multi_instance_fb, W },
    { "archive_global_library", "libraryName; optional targetDirectory, targetName, mode=Compressed",
      "Archive a global library (.zal21). targetDirectory defaults to the configured archives root - pass it only when the "
      "user names a different folder. mode: None, Compressed, DiscardRestorableData, DiscardRestorableDataAndCompressed.",
      a_archive_global_library, G },
    { "close_global_library", "libraryName; optional discardChanges=false",
      "Close an open global library. Refused with unsaved changes unless discardChanges=true.", a_close_global_library, G },
    { "compare_to_target", "targetLibrary; optional library='project', includeIdentical=false",
      "Compare two libraries (project or open global): per element only_in_source, only_in_target, differs, identical, and "
      "a summary. Read-only.",
      a_compare_to_target, G },
    { "create_global_library", "name; optional targetDirectory",
      "Create a new empty global library (.al21), open read-write. targetDirectory defaults to the configured libraries "
      "root - pass it only when the user names a different folder. Call save_global_library to persist.",
      a_create_global_library, G },
    { "create_library_folder", "folderPath; optional library='project', typeFilter=master_copies|types",
      "Create a folder (intermediate folders included; idempotent) in the master copies (default) or types section. Read-only "
      "global libraries are refused.",
      a_create_library_folder, GW },
    { "delete_library_folder", "folderPath; optional library='project', recursive=false, typeFilter=master_copies|types",
      "Delete a library folder. A non-empty folder needs recursive=true.", a_delete_library_folder, GW | AF_DESTRUCTIVE },
    { "delete_master_copy", "path; optional library='project'", "Delete a master copy by its 'Folder/Name' path.",
      a_delete_master_copy, GW | AF_DESTRUCTIVE },
    { "delete_unused_types", "optional library='project', path, mode=preserve_default|delete_all, dryRun=true, confirm",
      "List the library types without instances (project library: in the project PLCs; global library: without dependents). "
      "dryRun=false with confirm='yes' cleans up: preserve_default keeps the default version of unused types, delete_all "
      "removes them.",
      a_delete_unused_types, GW | AF_DESTRUCTIVE },
    { "export_type_xml", "library, path, outputPath; optional version='latest_committed', overwrite=false, returnInline",
      "Export a type version as SimaticML. After export, ask the user whether to open the file (admin action=open_file).",
      a_export_type_xml, G },
    { "find", "library, query; optional path, typeFilter=all|types|master_copies, caseSensitive=false, limit=500",
      "Search folders, types and master copies by name (glob * and ?; plain text = contains). Each match carries the "
      "get_tree envelope.",
      a_find, G },
    { "get_info", "library", "Library metadata (path, version, author, edit mode, unsaved changes), item counts and root folders.",
      a_get_info, G },
    { "get_master_copy", "library, path",
      "Master copy metadata and its contents (one row per object with its kind). A master copy carries no dependencies and "
      "has no XML export in Openness.",
      a_get_master_copy, G },
    { "get_tree", "library; optional path, depth=0, nested=true, includeItems=false, typeFilter=all|types|master_copies, limit=1000",
      "Folder tree of a library (depth 0 = unlimited, 1 = immediate children). includeItems=true adds types (kind, versions, "
      "default version) and master copies. nested=false prints flat 'Types/Folder/Name' paths usable as path arguments.",
      a_get_tree, G },
    { "get_type", "library, path; optional version='latest_committed', includeXml=false",
      "Type metadata, all versions (state, default, author, date), dependencies and dependents of the selected version. "
      "version: latest_committed, latest_any, default or a number. includeXml=true adds the SimaticML (first 30 KB).",
      a_get_type, G },
    { "instantiate_library_type", "deviceName, path; optional library='project', version='latest_committed'",
      "Create the block (or PLC data type) of a committed type version in the PLC. From a global library the type and its "
      "dependencies are copied into the project library too.",
      a_instantiate_library_type, W },
    { "instantiate_master_copy", "deviceName, path; optional library='project'",
      "Copy the content of a master copy into the PLC (blocks, PLC data types, tag tables or block folders, by content).",
      a_instantiate_master_copy, W },
    { "list_libraries", "",
      "The project library and the open global libraries (name to use as the library argument, edit mode, path), plus "
      "known libraries that are not open.",
      a_list_libraries, G },
    { "open_global_library", "optional path, openMode=ReadOnly|ReadWrite, upgrade=false",
      "Open a global library (.al21 file or its folder). Without path, lists the libraries under the configured libraries "
      "root. upgrade=true opens a library of an older TIA Portal version (TIA makes an upgraded copy).",
      a_open_global_library, G },
    { "promote_to_global", "targetLibraryName; optional sourceLibrary='project', path, forceUpdateMode, structureConflictMode, deleteUnusedVersions",
      "Copy types (all, or the type/folder at path) from the source library into a writable global library. "
      "forceUpdateMode: SetOnlyHigher (default), ForceSetAny, NoDefaultVersionChange. structureConflictMode: "
      "CancelIfConflicts (default), UpdateStructure, RetainStructure.",
      a_promote_to_global, GW },
    { "publish_block_as_master_copy", "deviceName, blockName; optional library='project', folderPath, onCollision=autoRename|replace|fail",
      "Publish an FB/FC/OB/DB as a master copy (also inconsistent blocks). Dependencies are not included.",
      a_publish_block_as_master_copy, W },
    { "publish_plc_type_as_master_copy", "deviceName, udtName; optional library='project', folderPath, onCollision",
      "Publish a PLC data type as a master copy.", a_publish_plc_type_as_master_copy, W },
    { "reload_global_library", "libraryName; optional discardChanges=false",
      "Read a global library again from disk after it was changed outside this session (close + open, same mode). Refused "
      "with unsaved changes unless discardChanges=true.",
      a_reload_global_library, G },
    { "rename_library_folder", "folderPath, newName; optional library='project', typeFilter=master_copies|types",
      "Rename a library folder (not the section root).", a_rename_library_folder, GW },
    { "rename_master_copy", "path, newName; optional library='project'",
      "Rename a master copy. The objects inside keep their names.", a_rename_master_copy, GW },
    { "rename_type", "path, newName; optional library='project'",
      "Rename a library type. Versions and blocks already instantiated are unchanged.", a_rename_type, GW },
    { "save_global_library", "libraryName", "Save an open global library to its .al21 file.", a_save_global_library, GW },
    { "update_check", "optional library='project', mode=ReportOutOfDateOnly|ReportOutOfDateAndUpToDate",
      "Preview of update_project: which instances in the project are out of date with the library. Read-only.", a_update_check, P },
    { "update_project", "optional library='project', path, deleteUnusedVersions=false",
      "Update all instances in the project PLCs to the library's default versions (all types, or the type/folder at path). "
      "WARNING: TIA Portal also deletes the instances of these types that nothing uses (blocks/data types not called or "
      "referenced); the result lists every removed object. Run update_check first; the call is one undo step in TIA.",
      a_update_project, W | AF_DESTRUCTIVE },
};

const tool_def tool_library = {
    .name = "library",
    .title = "Library management",
    .summary = "Project library and global libraries: browse and search types and master copies, type versions and "
               "dependencies, publish and instantiate, folders and renames, update check / update project, promote to a "
               "global library, compare, clean up, open/create/save/archive global libraries. library='project' (default) "
               "is the project library; other names are open global libraries (list_libraries).",
    .properties =
        "{"
        "\"library\":{\"type\":\"string\",\"description\":\"'project' (default) or the name/path of an open global library.\"},"
        "\"libraryName\":{\"type\":\"string\"},\"targetLibrary\":{\"type\":\"string\"},\"targetLibraryName\":{\"type\":\"string\"},"
        "\"sourceLibrary\":{\"type\":\"string\"},"
        "\"path\":{\"type\":\"string\",\"description\":\"'Folder/Sub/Name' inside the library (optionally prefixed by Types/ or "
        "Master copies/); open_global_library: .al21 file.\"},"
        "\"folderPath\":{\"type\":\"string\"},\"newName\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},"
        "\"query\":{\"type\":\"string\"},"
        "\"typeFilter\":{\"type\":\"string\",\"enum\":[\"all\",\"types\",\"master_copies\"]},"
        "\"caseSensitive\":{\"type\":\"boolean\"},\"limit\":{\"type\":\"integer\"},\"depth\":{\"type\":\"integer\"},"
        "\"nested\":{\"type\":\"boolean\"},\"includeItems\":{\"type\":\"boolean\"},\"includeXml\":{\"type\":\"boolean\"},"
        "\"includeIdentical\":{\"type\":\"boolean\"},"
        "\"version\":{\"type\":\"string\",\"description\":\"latest_committed (default), latest_any, default or a version number.\"},"
        "\"deviceName\":{\"type\":\"string\"},\"blockName\":{\"type\":\"string\"},\"udtName\":{\"type\":\"string\"},"
        "\"parentFB\":{\"type\":\"string\"},\"memberName\":{\"type\":\"string\"},"
        "\"onCollision\":{\"type\":\"string\",\"enum\":[\"autoRename\",\"replace\",\"fail\"]},"
        "\"openMode\":{\"type\":\"string\",\"enum\":[\"ReadOnly\",\"ReadWrite\"]},\"upgrade\":{\"type\":\"boolean\"},"
        "\"discardChanges\":{\"type\":\"boolean\"},\"targetDirectory\":{\"type\":\"string\"},\"targetName\":{\"type\":\"string\"},"
        "\"mode\":{\"type\":\"string\"},\"forceUpdateMode\":{\"type\":\"string\"},\"structureConflictMode\":{\"type\":\"string\"},"
        "\"deleteUnusedVersions\":{\"type\":\"boolean\"},\"recursive\":{\"type\":\"boolean\"},\"dryRun\":{\"type\":\"boolean\"},"
        "\"confirm\":{\"type\":\"string\"},\"outputPath\":{\"type\":\"string\"},\"overwrite\":{\"type\":\"boolean\"},"
        "\"returnInline\":{\"type\":\"boolean\"}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
};
