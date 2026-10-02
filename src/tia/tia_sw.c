#include "tia_sw.h"

#include "app/config.h"
#include "app/export_store.h"
#include "tia/session.h"
#include "tia/simaticml.h"
#include "util/fmt.h"
#include "util/fs.h"
#include "util/strbuf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INLINE_MAX 60000

typedef struct container_def {
    const char *name;
    const char *root_prop;
    const char *items[2];
} container_def;

static const container_def k_containers[SWC_COUNT] = {
    { "program_blocks", "BlockGroup", { "Blocks", NULL } },
    { "tag_tables", "TagTableGroup", { "TagTables", NULL } },
    { "udts", "TypeGroup", { "Types", NULL } },
    { "watch_tables", "WatchAndForceTableGroup", { "WatchTables", "ForceTables" } },
    { "external_sources", "ExternalSourceGroup", { "ExternalSources", NULL } },
    { "technology_objects", "TechnologicalObjectGroup", { "TechnologicalObjects", NULL } },
};

const char *sw_container_name(sw_container c)
{
    return c < SWC_COUNT ? k_containers[c].name : "?";
}

int sw_container_parse(const char *name, sw_container *out)
{
    static const struct {
        const char *alias;
        sw_container c;
    } aliases[] = {
        { "program_blocks", SWC_BLOCKS }, { "blocks", SWC_BLOCKS },         { "tag_tables", SWC_TAG_TABLES },
        { "tags", SWC_TAG_TABLES },       { "udts", SWC_TYPES },            { "types", SWC_TYPES },
        { "plc_data_types", SWC_TYPES },  { "watch_tables", SWC_WATCH_TABLES }, { "watch", SWC_WATCH_TABLES },
        { "external_sources", SWC_EXTERNAL_SOURCES }, { "technology_objects", SWC_TECH_OBJECTS },
    };
    if (!name)
        return -1;
    for (size_t i = 0; i < sizeof aliases / sizeof aliases[0]; i++) {
        if (_stricmp(name, aliases[i].alias) == 0) {
            *out = aliases[i].c;
            return 0;
        }
    }
    return -1;
}

th sw_container_root(th plc_software, sw_container c)
{
    return c < SWC_COUNT ? td_get_h(plc_software, k_containers[c].root_prop) : 0;
}

int sw_walk_group(th group, sw_container c, const char *attrs_csv, const char *folder, int depth, sw_item_fn item_fn,
                  sw_folder_fn folder_fn, void *ctx)
{
    int stop = 0;
    if (folder_fn && (stop = folder_fn(ctx, group, folder, depth)) != 0)
        return stop;
    if (item_fn) {
        for (int k = 0; k < 2 && k_containers[c].items[k] && !stop; k++) {
            th coll = td_get_h(group, k_containers[c].items[k]);
            if (!coll)
                continue;
            cJSON *items = td_enum(coll, attrs_csv, -1);
            const cJSON *it;
            cJSON_ArrayForEach(it, items)
            {
                if ((stop = item_fn(ctx, it, folder)) != 0)
                    break;
            }
            cJSON_Delete(items);
        }
    }
    if (stop || depth > 32)
        return stop;
    th groups = td_get_h(group, "Groups");
    cJSON *gl = groups ? td_enum(groups, "Name", -1) : NULL;
    const cJSON *g;
    cJSON_ArrayForEach(g, gl)
    {
        char sub[1024];
        const char *name = tdi_s(g, "Name");
        snprintf(sub, sizeof sub, "%s%s%s", folder, *folder ? "/" : "", name ? name : "?");
        if ((stop = sw_walk_group(tdv_h(g), c, attrs_csv, sub, depth + 1, item_fn, folder_fn, ctx)) != 0)
            break;
    }
    cJSON_Delete(gl);
    return stop;
}

int sw_walk(th plc_software, sw_container c, const char *attrs_csv, sw_item_fn item_fn, sw_folder_fn folder_fn, void *ctx)
{
    th root = sw_container_root(plc_software, c);
    if (!root)
        return -1;
    return sw_walk_group(root, c, attrs_csv, "", 0, item_fn, folder_fn, ctx);
}

/* ---- find by name ------------------------------------------------------------ */

typedef struct find_state {
    const char *want_name;   /* last path segment */
    const char *want_folder; /* folder part or NULL */
    sw_found *out;
    int exact;
    int ci_hits;
    sw_found ci;
    strbuf similar;
    int nsimilar;
} find_state;

static int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0)
        return 0;
    for (; *hay; hay++)
        if (_strnicmp(hay, needle, n) == 0)
            return 1;
    return 0;
}

static void fill_found(sw_found *f, const cJSON *item, const char *folder)
{
    f->item = tdv_h(item);
    snprintf(f->name, sizeof f->name, "%s", tdi_s(item, "Name") ? tdi_s(item, "Name") : "");
    snprintf(f->folder, sizeof f->folder, "%s", folder);
    const char *t = tdv_type(item);
    const char *dot = t ? strrchr(t, '.') : NULL;
    snprintf(f->type, sizeof f->type, "%s", dot ? dot + 1 : (t ? t : "?"));
}

static int find_cb(void *ctx, const cJSON *item, const char *folder)
{
    find_state *s = ctx;
    const char *name = tdi_s(item, "Name");
    if (!name)
        return 0;
    int folder_ok = !s->want_folder || _stricmp(folder, s->want_folder) == 0;
    if (folder_ok && strcmp(name, s->want_name) == 0) {
        fill_found(s->out, item, folder);
        s->exact = 1;
        return 1;
    }
    if (folder_ok && _stricmp(name, s->want_name) == 0) {
        if (!s->ci_hits)
            fill_found(&s->ci, item, folder);
        s->ci_hits++;
    } else if (s->nsimilar < 8 && (contains_ci(name, s->want_name) || contains_ci(s->want_name, name))) {
        sb_printf(&s->similar, "%s%s%s%s", s->similar.len ? ", " : "", folder, *folder ? "/" : "", name);
        s->nsimilar++;
    }
    return 0;
}

int sw_find(tool_ctx *c, th plc_software, sw_container cont, const char *name, sw_found *out)
{
    memset(out, 0, sizeof *out);
    char folder[512] = "";
    const char *leaf = name;
    const char *slash = strrchr(name, '/');
    if (slash) {
        size_t n = (size_t)(slash - name);
        if (n >= sizeof folder)
            n = sizeof folder - 1;
        memcpy(folder, name, n);
        folder[n] = 0;
        leaf = slash + 1;
    }
    /* Accept quoted names: "Motor_DB" */
    char unq[256];
    size_t ln = strlen(leaf);
    if (ln >= 2 && leaf[0] == '"' && leaf[ln - 1] == '"' && ln - 2 < sizeof unq) {
        memcpy(unq, leaf + 1, ln - 2);
        unq[ln - 2] = 0;
        leaf = unq;
    }
    find_state s;
    memset(&s, 0, sizeof s);
    s.want_name = leaf;
    s.want_folder = slash ? folder : NULL;
    s.out = out;
    sb_init(&s.similar);
    sw_walk(plc_software, cont, "Name", find_cb, NULL, &s);
    int rc = 0;
    if (!s.exact) {
        if (s.ci_hits == 1) {
            *out = s.ci;
        } else {
            static const char *what[SWC_COUNT] = { "block", "tag table", "PLC data type", "watch/force table",
                                                   "external source", "technology object" };
            rc = fail(c, "%s '%s' not found%s%s%s", what[cont], name, s.similar.len ? ". Similar: " : "",
                      sb_str(&s.similar), s.ci_hits > 1 ? " (several case-insensitive matches; use the exact name)" : "");
        }
    }
    sb_free(&s.similar);
    return rc;
}

/* ---- folders ------------------------------------------------------------------ */

static th find_child_group(th group, const char *name, int *ok)
{
    th groups = td_get_h(group, "Groups");
    *ok = groups != 0;
    if (!groups)
        return 0;
    cJSON *gl = td_enum(groups, "Name", -1);
    th found = 0;
    const cJSON *g;
    cJSON_ArrayForEach(g, gl)
    {
        const char *n = tdi_s(g, "Name");
        if (n && _stricmp(n, name) == 0) {
            found = tdv_h(g);
            break;
        }
    }
    cJSON_Delete(gl);
    return found;
}

th sw_folder(tool_ctx *c, th plc_software, sw_container cont, const char *path, int create, int create_parents)
{
    th group = sw_container_root(plc_software, cont);
    if (!group) {
        fail_td(c, "cannot read the container root");
        return 0;
    }
    if (!path)
        return group;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    /* Normalise separators and trim leading/trailing slashes. */
    for (char *p = buf; *p; p++)
        if (*p == '\\')
            *p = '/';
    char *seg = buf;
    while (*seg == '/')
        seg++;
    char walked[1024] = "";
    while (*seg) {
        char *end = strchr(seg, '/');
        if (end)
            *end = 0;
        if (*seg) {
            int ok = 0;
            th next = find_child_group(group, seg, &ok);
            int is_last = !end || !end[1];
            if (!next) {
                if (!create || (!is_last && !create_parents)) {
                    fail(c, "folder '%s%s%s' does not exist in %s%s", walked, *walked ? "/" : "", seg, sw_container_name(cont),
                         create ? " (pass createParents=true to create missing parent folders)" : "");
                    return 0;
                }
                th groups = td_get_h(group, "Groups");
                next = groups ? td_call_h(groups, "Create", tda("s", seg)) : 0;
                if (!next) {
                    fail_td(c, "creating folder failed");
                    return 0;
                }
                out(c, "Created folder %s%s%s\n", walked, *walked ? "/" : "", seg);
            }
            group = next;
            size_t wl = strlen(walked);
            snprintf(walked + wl, sizeof walked - wl, "%s%s", wl ? "/" : "", seg);
        }
        if (!end)
            break;
        seg = end + 1;
    }
    return group;
}

/* ---- export ----------------------------------------------------------------- */

int sw_export_xml(tool_ctx *c, th obj, char *path_out, size_t cap)
{
    if (fs_temp_path("export", ".xml", path_out, cap) != 0)
        return fail(c, "cannot create a temporary file");
    if (td_call_v(obj, "Export", tda("fe", path_out, "Siemens.Engineering.ExportOptions", "WithDefaults")) == 0)
        return 0;
    fs_remove(path_out);
    if (strstr(td_err(), "nconsistent")) {
        /* Freshly imported or edited objects must be compiled before TIA exports them. */
        th comp = td_service(obj, "Siemens.Engineering.Compiler.ICompilable");
        th result = comp ? session_compile(comp) : 0;
        if (result && td_call_v(obj, "Export", tda("fe", path_out, "Siemens.Engineering.ExportOptions", "WithDefaults")) == 0) {
            out(c, "(compiled %s first: it was inconsistent)\n", "the object");
            return 0;
        }
        fs_remove(path_out);
        return fail_td(c, "export failed even after compiling (fix the compile errors: blocks_read action=compile_block)");
    }
    return fail_td(c, "export failed");
}

int sw_plc(tool_ctx *c, nav_plc *plc)
{
    const char *dev = arg_req(c, "deviceName");
    if (!dev)
        return -1;
    snprintf(c->error_context, sizeof c->error_context, "device=%s", dev);
    return nav_find_plc(c, session_project(), dev, plc);
}

int sw_deliver(tool_ctx *c, const char *src_file, const char *default_name, const char *content_type,
               const char *output_path, int return_inline)
{
    long long size = fs_file_size(src_file);
    if (output_path && *output_path) {
        char full[TC_PATH_MAX], target[TC_PATH_MAX];
        if (fs_full_path(output_path, full, sizeof full) != 0)
            return fail(c, "invalid outputPath '%s'", output_path);
        size_t n = strlen(full);
        int is_dir = fs_is_dir(full) || (n > 0 && (full[n - 1] == '\\' || full[n - 1] == '/')) || !strchr(fs_basename(full), '.');
        if (is_dir) {
            fs_mkdirs(full);
            fs_join(target, sizeof target, full, default_name);
        } else {
            snprintf(target, sizeof target, "%s", full);
            char parent[TC_PATH_MAX];
            snprintf(parent, sizeof parent, "%s", target);
            char *slash = strrchr(parent, '\\');
            if (slash) {
                *slash = 0;
                fs_mkdirs(parent);
            }
        }
        if (fs_copy(src_file, target, 1) != 0)
            return fail(c, "cannot write '%s'", target);
        char sz[32];
        fmt_size(size, sz, sizeof sz);
        out(c, "Written %s (%s). Ask the user whether to open it (admin action=open_file).\n", target, sz);
        return 0;
    }
    if (return_inline && size >= 0 && size <= INLINE_MAX) {
        char *data = NULL;
        size_t len = 0;
        if (fs_read_all(src_file, &data, &len) != 0)
            return fail(c, "cannot read the exported file");
        const char *p = data;
        if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
            p += 3;
        out_raw(c, p);
        if (len && data[len - 1] != '\n')
            out_raw(c, "\n");
        free(data);
        return 0;
    }
    export_info e;
    const char *tool = c->tool ? c->tool->name : "";
    const char *action = c->action ? c->action->name : "";
    if (export_put_file(tool, action, default_name, content_type, src_file, &e) != 0)
        return fail(c, "cannot store the export");
    char sz[32];
    fmt_size(e.size, sz, sizeof sz);
    out(c, "Content is %s, stored as exportId=%s (name %s). Read it with admin action=get_export exportId=%s (paged) or "
           "write it to disk with admin action=save_export.\n",
        sz, e.id, e.name, e.id);
    return 0;
}

mxml_node_t *sw_export_tree(tool_ctx *c, th obj)
{
    char path[TC_PATH_MAX];
    if (sw_export_xml(c, obj, path, sizeof path) != 0)
        return NULL;
    char err[256];
    mxml_node_t *top = sml_load_file(path, err, sizeof err);
    fs_remove(path);
    if (!top)
        fail(c, "cannot parse the exported XML: %s", err);
    return top;
}
