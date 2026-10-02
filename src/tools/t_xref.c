/* xref: cross-references, callers and call graph of a PLC program. */
#include "tools.h"

#include "tia/tia_dyn.h"
#include "tia/tia_sw.h"
#include "util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T_XREF_SERVICE "Siemens.Engineering.CrossReference.CrossReferenceService"
#define T_XREF_FILTER "Siemens.Engineering.CrossReference.CrossReferenceFilter"
#define REF_ATTRS "Name,TypeName,Address,Path,Device"
#define LOC_ATTRS "Name,Access,ReferenceType,ReferenceLocation,Address,ReferencedAsName"

/* Runs the cross-reference service on obj; returns the Sources composition or 0. */
static th xref_sources(tool_ctx *c, th obj, const char *filter)
{
    th svc = td_service(obj, T_XREF_SERVICE);
    if (!svc) {
        if (td_failed())
            fail_td(c, "cross-reference service unavailable");
        else
            fail(c, "this object has no cross-references");
        return 0;
    }
    th result = td_call_h(svc, "GetCrossReferences", tda("e", T_XREF_FILTER, filter && *filter ? filter : "AllObjects"));
    if (!result) {
        fail_td(c, "GetCrossReferences failed");
        return 0;
    }
    return td_get_h(result, "Sources");
}

/* ---- call graph ------------------------------------------------------------------ */

typedef struct gnode {
    char name[256];
    char folder[512];
    char type[32];
    long long number;
    int is_code;
    int *callees;
    int ncallees, capcallees;
    int called;
    char instance_of[256]; /* instance DBs */
} gnode;

typedef struct graph {
    gnode *n;
    int count, cap;
} graph;

static int g_find(graph *g, const char *name)
{
    for (int i = 0; i < g->count; i++)
        if (_stricmp(g->n[i].name, name) == 0)
            return i;
    return -1;
}

static void g_edge(graph *g, int from, int to)
{
    gnode *a = &g->n[from];
    for (int i = 0; i < a->ncallees; i++)
        if (a->callees[i] == to)
            return;
    if (a->ncallees == a->capcallees) {
        int cap = a->capcallees ? a->capcallees * 2 : 8;
        int *p = realloc(a->callees, (size_t)cap * sizeof *p);
        if (!p)
            return;
        a->callees = p;
        a->capcallees = cap;
    }
    a->callees[a->ncallees++] = to;
    g->n[to].called++;
}

static void g_free(graph *g)
{
    for (int i = 0; i < g->count; i++)
        free(g->n[i].callees);
    free(g->n);
}

static int collect_block(void *ctx, const cJSON *item, const char *folder)
{
    graph *g = ctx;
    if (g->count == g->cap) {
        int cap = g->cap ? g->cap * 2 : 64;
        gnode *p = realloc(g->n, (size_t)cap * sizeof *p);
        if (!p)
            return 1;
        g->n = p;
        g->cap = cap;
    }
    gnode *n = &g->n[g->count++];
    memset(n, 0, sizeof *n);
    snprintf(n->name, sizeof n->name, "%s", tdi_s(item, "Name") ? tdi_s(item, "Name") : "?");
    snprintf(n->folder, sizeof n->folder, "%s", folder);
    snprintf(n->type, sizeof n->type, "%s", short_type(item));
    n->number = tdi_i(item, "Number", 0);
    n->is_code = strcmp(n->type, "OB") == 0 || strcmp(n->type, "FB") == 0 || strcmp(n->type, "FC") == 0;
    return 0;
}

static int is_call_access(const char *access)
{
    return access && (strcmp(access, "Call") == 0 || strcmp(access, "UC") == 0 || strcmp(access, "CC") == 0 ||
                      strcmp(access, "Multiinstance") == 0 || strcmp(access, "MultiinstanceAndSymbol") == 0 ||
                      strcmp(access, "InstanceDB") == 0 || strcmp(access, "InstanceAndSymbol") == 0 ||
                      strcmp(access, "Parameterinstance") == 0 || strcmp(access, "ParameterinstanceAndSymbol") == 0);
}

/* Adds the CALL edges of one code block (references it Uses with a call access). */
static void add_edges(graph *g, int from, th block)
{
    tool_ctx probe;
    ctx_init(&probe, NULL, NULL);
    th sources = xref_sources(&probe, block, "AllObjects");
    ctx_free(&probe);
    if (!sources)
        return;
    cJSON *srcs = td_enum(sources, "Name", -1);
    const cJSON *s;
    cJSON_ArrayForEach(s, srcs)
    {
        cJSON *refs = td_enum(td_get_h(tdv_h(s), "References"), "Name", -1);
        const cJSON *r;
        cJSON_ArrayForEach(r, refs)
        {
            const char *name = tdi_s(r, "Name");
            int to = name ? g_find(g, name) : -1;
            if (to < 0 || to == from)
                continue;
            cJSON *locs = td_enum(td_get_h(tdv_h(r), "Locations"), "Access,ReferenceType", -1);
            const cJSON *l;
            cJSON_ArrayForEach(l, locs)
            {
                const char *rt = tdi_s(l, "ReferenceType");
                if ((!rt || strcmp(rt, "Uses") == 0) && is_call_access(tdi_s(l, "Access"))) {
                    /* An instance DB used by a call belongs to the call: link to its FB instead. */
                    g_edge(g, from, to);
                    break;
                }
            }
            cJSON_Delete(locs);
        }
        cJSON_Delete(refs);
    }
    cJSON_Delete(srcs);
}

typedef struct build_ctx {
    tool_ctx *c;
    graph *g;
} build_ctx;

static int build_edges_cb(void *ctx, const cJSON *item, const char *folder)
{
    (void)folder;
    build_ctx *b = ctx;
    if (cancelled(b->c))
        return 1;
    int i = g_find(b->g, tdi_s(item, "Name") ? tdi_s(item, "Name") : "");
    if (i >= 0 && b->g->n[i].is_code) {
        add_edges(b->g, i, tdv_h(item));
        progress(b->c, i + 1, b->g->count, b->g->n[i].name);
    }
    if (i >= 0 && strcmp(b->g->n[i].type, "InstanceDB") == 0) {
        char *of = td_get_s(tdv_h(item), "InstanceOfName");
        snprintf(b->g->n[i].instance_of, sizeof b->g->n[i].instance_of, "%s", of ? of : "");
        free(of);
    }
    return 0;
}

static int build_graph(tool_ctx *c, nav_plc *plc, graph *g)
{
    memset(g, 0, sizeof *g);
    if (sw_walk(plc->software, SWC_BLOCKS, "Name,Number", collect_block, NULL, g) != 0 && td_failed())
        return fail_td(c, "reading the blocks failed");
    build_ctx b = { c, g };
    sw_walk(plc->software, SWC_BLOCKS, "Name", build_edges_cb, NULL, &b);
    /* A single-instance call appears as a use of the instance DB: also link
       the caller to the FB that owns the DB. */
    for (int i = 0; i < g->count; i++) {
        int n = g->n[i].ncallees;
        for (int k = 0; k < n; k++) {
            const gnode *db = &g->n[g->n[i].callees[k]];
            int fb = db->instance_of[0] ? g_find(g, db->instance_of) : -1;
            if (fb >= 0 && fb != i)
                g_edge(g, i, fb);
        }
    }
    return 0;
}

static void node_label(const gnode *n, char *out, size_t cap)
{
    const char *prefix = strstr(n->type, "DB") ? "DB" : n->type;
    snprintf(out, cap, "%s%s%s [%s%lld]", n->folder, *n->folder ? "/" : "", n->name, prefix, n->number);
}

static int resolve_target(tool_ctx *c, graph *g, const char *name)
{
    const char *leaf = strrchr(name, '/');
    leaf = leaf ? leaf + 1 : name;
    int i = g_find(g, leaf);
    if (i < 0)
        fail(c, "block '%s' not found", name);
    return i;
}

/* ---- actions ----------------------------------------------------------------------- */

/* Prints the locations of one reference aggregated as
   "Uses Read x3, Write x2 @ NW1 (title); @ Program code". */
static void print_locations(tool_ctx *c, th ref, const char *indent, const char *only_type)
{
    cJSON *locs = td_enum(td_get_h(ref, "Locations"), LOC_ATTRS, 500);
    typedef struct { char key[96]; int n; } bucket;
    bucket kinds[16];
    int nk = 0;
    strbuf where;
    sb_init(&where);
    int nwhere = 0;
    const cJSON *l;
    char rtype[32] = "";
    cJSON_ArrayForEach(l, locs)
    {
        const char *rt = tdi_s(l, "ReferenceType");
        if (only_type && rt && strcmp(rt, only_type) != 0)
            continue;
        snprintf(rtype, sizeof rtype, "%s", rt ? rt : "?");
        const char *acc = tdi_s(l, "Access");
        int k;
        for (k = 0; k < nk; k++)
            if (strcmp(kinds[k].key, acc ? acc : "?") == 0)
                break;
        if (k == nk && nk < 16) {
            snprintf(kinds[nk].key, sizeof kinds[nk].key, "%s", acc ? acc : "?");
            kinds[nk++].n = 0;
        }
        if (k < nk)
            kinds[k].n++;
        const char *loc = tdi_s(l, "ReferenceLocation");
        if (loc && *loc && !strstr(sb_str(&where), loc) && nwhere < 6) {
            sb_printf(&where, "%s%s", nwhere ? "; " : "", loc);
            nwhere++;
        }
    }
    if (nk) {
        out(c, "%s%s ", indent, rtype);
        for (int k = 0; k < nk; k++)
            out(c, "%s%s x%d", k ? ", " : "", kinds[k].key, kinds[k].n);
        if (where.len)
            out(c, " %s %s%s", nwhere > 1 ? "at" : "at", sb_str(&where), nwhere >= 6 ? "; ..." : "");
        out(c, "\n");
    }
    sb_free(&where);
    cJSON_Delete(locs);
}

static int a_get_references(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *name = arg_req(c, "blockName");
    if (!name)
        return -1;
    sw_found b;
    if (sw_find(c, plc.software, SWC_BLOCKS, name, &b) != 0)
        return -1;
    th sources = xref_sources(c, b.item, arg_s(c, "filter"));
    if (!sources)
        return -1;
    cJSON *srcs = td_enum(sources, "Name,TypeName,Address", -1);
    const cJSON *s;
    int nrefs = 0;
    cJSON_ArrayForEach(s, srcs)
    {
        out(c, "%s  [type=%s%s%s]\n", tdi_s(s, "Name") ? tdi_s(s, "Name") : "?", tdi_s(s, "TypeName") ? tdi_s(s, "TypeName") : "?",
            tdi_s(s, "Address") && *tdi_s(s, "Address") ? ", address=" : "", tdi_s(s, "Address") ? tdi_s(s, "Address") : "");
        cJSON *refs = td_enum(td_get_h(tdv_h(s), "References"), REF_ATTRS, -1);
        const cJSON *r;
        cJSON_ArrayForEach(r, refs)
        {
            const char *addr = tdi_s(r, "Address");
            out(c, "  %s  [type=%s%s%s]\n", tdi_s(r, "Name") ? tdi_s(r, "Name") : "?",
                tdi_s(r, "TypeName") ? tdi_s(r, "TypeName") : "?", addr && *addr ? ", address=" : "", addr && *addr ? addr : "");
            print_locations(c, tdv_h(r), "      ", NULL);
            nrefs++;
        }
        cJSON_Delete(refs);
    }
    cJSON_Delete(srcs);
    out(c, "%d referenced object(s). Uses = this block uses the object, UsedBy = the object uses this block.\n", nrefs);
    return 0;
}

/* find_callers: who uses a block or a PLC tag. */
static int tag_search(void *ctx, const cJSON *item, const char *folder);

typedef struct tag_find {
    const char *name;
    th tag;
    char table[256];
} tag_find;

static int a_find_callers(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *name = arg_req(c, "blockName");
    if (!name)
        return -1;
    th target = 0;
    char label[600];
    sw_found b;
    tool_ctx probe;
    ctx_init(&probe, c->tool, NULL);
    if (sw_find(&probe, plc.software, SWC_BLOCKS, name, &b) == 0) {
        target = b.item;
        snprintf(label, sizeof label, "block %s", b.name);
    } else {
        tag_find t = { name, 0, "" };
        if (name[0] == '"') {
            static char unq[256];
            snprintf(unq, sizeof unq, "%.*s", (int)strlen(name) - 2, name + 1);
            t.name = unq;
        }
        sw_walk(plc.software, SWC_TAG_TABLES, "Name", tag_search, NULL, &t);
        if (t.tag) {
            target = t.tag;
            snprintf(label, sizeof label, "PLC tag %s (table %s)", t.name, t.table);
        }
    }
    if (!target) {
        out_raw(c, sb_str(&probe.out));
        c->is_error = 1;
        ctx_free(&probe);
        out(c, "(no PLC tag with that name either)\n");
        return -1;
    }
    ctx_free(&probe);
    th sources = xref_sources(c, target, "AllObjects");
    if (!sources)
        return -1;
    out(c, "Users of %s:\n", label);
    int n = 0;
    cJSON *srcs = td_enum(sources, "Name", -1);
    const cJSON *s;
    cJSON_ArrayForEach(s, srcs)
    {
        cJSON *refs = td_enum(td_get_h(tdv_h(s), "References"), REF_ATTRS, -1);
        const cJSON *r;
        cJSON_ArrayForEach(r, refs)
        {
            /* Only objects that use the target (ReferenceType UsedBy). */
            cJSON *locs = td_enum(td_get_h(tdv_h(r), "Locations"), "ReferenceType", -1);
            int used_by = 0;
            const cJSON *l;
            cJSON_ArrayForEach(l, locs)
            {
                const char *rt = tdi_s(l, "ReferenceType");
                used_by |= rt && strcmp(rt, "UsedBy") == 0;
            }
            cJSON_Delete(locs);
            if (!used_by)
                continue;
            const char *addr = tdi_s(r, "Address");
            out(c, "%s  [type=%s%s%s]\n", tdi_s(r, "Name") ? tdi_s(r, "Name") : "?", tdi_s(r, "TypeName") ? tdi_s(r, "TypeName") : "?",
                addr && *addr ? ", address=" : "", addr && *addr ? addr : "");
            print_locations(c, tdv_h(r), "    ", "UsedBy");
            n++;
        }
        cJSON_Delete(refs);
    }
    cJSON_Delete(srcs);
    out(c, "%d user(s).\n", n);
    return 0;
}

static int tag_search(void *ctx, const cJSON *item, const char *folder)
{
    (void)folder;
    tag_find *t = ctx;
    th tags = td_get_h(tdv_h(item), "Tags");
    th found = tags ? td_call_h(tags, "Find", tda("s", t->name)) : 0;
    if (found) {
        t->tag = found;
        snprintf(t->table, sizeof t->table, "%s", tdi_s(item, "Name") ? tdi_s(item, "Name") : "?");
        return 1;
    }
    td_clear_err();
    return 0;
}

static void print_tree(tool_ctx *c, graph *g, int i, int depth, int max_depth, int *stack, int *lines)
{
    char label[900];
    node_label(&g->n[i], label, sizeof label);
    if (*lines >= 2000)
        return;
    (*lines)++;
    for (int k = 0; k < depth; k++)
        if (stack[k] == i) {
            out(c, "%*s%s (recursive)\n", depth * 2, "", label);
            return;
        }
    out(c, "%*s%s\n", depth * 2, "", label);
    if (depth >= max_depth) {
        if (g->n[i].ncallees)
            out(c, "%*s... (maxDepth reached)\n", depth * 2 + 2, "");
        return;
    }
    stack[depth] = i;
    for (int k = 0; k < g->n[i].ncallees; k++) {
        int to = g->n[i].callees[k];
        if (strstr(g->n[to].type, "DB"))
            continue; /* instance DBs are shown through their FB */
        print_tree(c, g, to, depth + 1, max_depth, stack, lines);
    }
}

static int a_get_call_tree(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    graph g;
    if (build_graph(c, &plc, &g) != 0)
        return -1;
    int max_depth = (int)arg_i(c, "maxDepth", 10);
    if (max_depth < 1 || max_depth > 64)
        max_depth = 10;
    int stack[64];
    int lines = 0;
    const char *root = arg_s(c, "blockName");
    if (root && *root) {
        int i = resolve_target(c, &g, root);
        if (i >= 0)
            print_tree(c, &g, i, 0, max_depth, stack, &lines);
    } else {
        for (int i = 0; i < g.count; i++)
            if (strcmp(g.n[i].type, "OB") == 0)
                print_tree(c, &g, i, 0, max_depth, stack, &lines);
    }
    if (lines >= 2000)
        out(c, "(truncated at 2000 lines)\n");
    g_free(&g);
    return c->is_error ? -1 : 0;
}

static void find_paths(tool_ctx *c, graph *g, int cur, int target, int *path, int depth, int max_depth, int *found)
{
    path[depth] = cur;
    if (cur == target) {
        if (*found < 200) {
            for (int k = 0; k <= depth; k++) {
                char label[900];
                node_label(&g->n[path[k]], label, sizeof label);
                out(c, "%s%s", k ? " -> " : "", label);
            }
            out(c, "\n");
        }
        (*found)++;
        return;
    }
    if (depth >= max_depth)
        return;
    for (int k = 0; k < g->n[cur].ncallees; k++) {
        int to = g->n[cur].callees[k];
        int loop = 0;
        for (int j = 0; j <= depth; j++)
            loop |= path[j] == to;
        if (!loop)
            find_paths(c, g, to, target, path, depth + 1, max_depth, found);
    }
}

static int a_find_call_paths(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *name = arg_req(c, "blockName");
    if (!name)
        return -1;
    graph g;
    if (build_graph(c, &plc, &g) != 0)
        return -1;
    int target = resolve_target(c, &g, name);
    if (target < 0) {
        g_free(&g);
        return -1;
    }
    int max_depth = (int)arg_i(c, "maxDepth", 10);
    if (max_depth < 1 || max_depth > 64)
        max_depth = 10;
    int path[65];
    int found = 0;
    for (int i = 0; i < g.count; i++)
        if (strcmp(g.n[i].type, "OB") == 0)
            find_paths(c, &g, i, target, path, 0, max_depth, &found);
    if (!found)
        out(c, "No call path from any OB reaches %s (unused, or called indirectly).\n", name);
    else
        out(c, "%d path(s)%s.\n", found, found > 200 ? " (first 200 shown)" : "");
    g_free(&g);
    return 0;
}

static void mark_reachable(graph *g, int i, char *seen)
{
    if (seen[i])
        return;
    seen[i] = 1;
    for (int k = 0; k < g->n[i].ncallees; k++)
        mark_reachable(g, g->n[i].callees[k], seen);
}

static int a_find_unused(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    graph g;
    if (build_graph(c, &plc, &g) != 0)
        return -1;
    char *seen = calloc((size_t)g.count + 1, 1);
    for (int i = 0; i < g.count; i++)
        if (strcmp(g.n[i].type, "OB") == 0)
            mark_reachable(&g, i, seen);
    int n = 0;
    out(c, "Code blocks not reached from any OB call chain:\n");
    for (int i = 0; i < g.count; i++) {
        if (!g.n[i].is_code || seen[i])
            continue;
        char label[900];
        node_label(&g.n[i], label, sizeof label);
        out(c, "%s%s\n", label, g.n[i].called ? "  (called only by other unreachable blocks)" : "");
        n++;
    }
    if (!n)
        out(c, "(none)\n");
    out(c, "%d unused code block(s). Data blocks are not evaluated (they can be accessed from HMI/OPC UA).\n", n);
    free(seen);
    g_free(&g);
    return 0;
}

static int collect_type_name(void *ctx, const cJSON *item, const char *folder)
{
    (void)folder;
    strbuf *names = ctx;
    sb_printf(names, "|%s|", tdi_s(item, "Name") ? tdi_s(item, "Name") : "");
    return 0;
}

static int a_find_orphaned_instance_dbs(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    graph g;
    if (build_graph(c, &plc, &g) != 0)
        return -1;
    /* DBs derived from a PLC data type are listed as instance DBs of that type. */
    strbuf types;
    sb_init(&types);
    sw_walk(plc.software, SWC_TYPES, "Name", collect_type_name, NULL, &types);
    int n = 0, total = 0;
    out(c, "Instance DBs:\n");
    for (int i = 0; i < g.count; i++) {
        if (strcmp(g.n[i].type, "InstanceDB") != 0)
            continue;
        total++;
        const char *of = g.n[i].instance_of;
        int owner = *of ? g_find(&g, of) : -1;
        const char *problem = NULL;
        char key[300];
        snprintf(key, sizeof key, "|%s|", of);
        if (!*of)
            problem = "no InstanceOfName";
        else if (owner < 0 && strstr(sb_str(&types), key))
            problem = NULL; /* DB of a PLC data type */
        else if (owner < 0)
            problem = "owning FB not found in the program (system/library FB or deleted)";
        else if (!g.n[i].called)
            problem = "not used by any call";
        if (problem) {
            char label[900];
            node_label(&g.n[i], label, sizeof label);
            out(c, "%s  [instanceOf=%s]: %s\n", label, *of ? of : "?", problem);
            n++;
        }
    }
    out(c, "%d of %d instance DB(s) look orphaned.\n", n, total);
    sb_free(&types);
    g_free(&g);
    return 0;
}

static const action_def actions[] = {
    { "find_call_paths", "deviceName, blockName; optional maxDepth=10",
      "All call paths from OB roots to the target block, e.g. Main [OB1] -> ModbusPoll [FB2] -> mb_query [FB8].",
      a_find_call_paths, AF_PROJECT },
    { "find_callers", "deviceName, blockName",
      "Reverse lookup: which blocks reference the target. The target may be a BLOCK (call sites) or a PLC TAG (reading and "
      "writing blocks with Access=Read/Write and location).",
      a_find_callers, AF_PROJECT },
    { "find_orphaned_instance_dbs", "deviceName",
      "Instance DBs whose owning FB is missing or that no call uses.", a_find_orphaned_instance_dbs, AF_PROJECT },
    { "find_unused", "deviceName",
      "Code blocks not called from any OB chain (runs the cross-reference service on every block).", a_find_unused,
      AF_PROJECT },
    { "get_call_tree", "deviceName; optional blockName, maxDepth=10",
      "Forward CALL tree: what a root calls, recursively (default roots: all OBs).", a_get_call_tree, AF_PROJECT },
    { "get_references", "deviceName, blockName; optional filter=AllObjects|ObjectsWithReferences|ObjectsWithoutReferences|UnusedObjects",
      "Cross-references of one block: referenced objects with type, address, access (Read/Write/Call...) and location.",
      a_get_references, AF_PROJECT },
};

const tool_def tool_xref = {
    .name = "xref",
    .title = "Cross-references",
    .summary = "Cross-reference analysis of a PLC program: references of a block, callers of a block or tag, call tree, "
               "call paths, unused blocks, orphaned instance DBs.",
    .properties =
        "{"
        "\"deviceName\":{\"type\":\"string\",\"description\":\"Device or PLC name (session list_devices).\"},"
        "\"blockName\":{\"type\":\"string\",\"description\":\"Block name (bare or folder-qualified); find_callers also accepts a PLC tag name.\"},"
        "\"maxDepth\":{\"type\":\"integer\",\"description\":\"Maximum call depth (default 10).\"},"
        "\"filter\":{\"type\":\"string\",\"enum\":[\"AllObjects\",\"ObjectsWithReferences\",\"ObjectsWithoutReferences\",\"UnusedObjects\"],\"description\":\"get_references: cross-reference filter.\"}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
    .hints = TH_READONLY | TH_IDEMPOTENT,
};
