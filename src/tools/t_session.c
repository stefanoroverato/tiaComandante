/* session: TIA Portal connection and project lifecycle. */
#include "tools.h"

#include "app/config.h"
#include "mcp/registry.h"
#include "tc_version.h"
#include "tia/session.h"
#include "tia/tia_dyn.h"
#include "tia/tia_nav.h"
#include "util/fs.h"
#include "util/strbuf.h"

#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- shared helpers ---------------------------------------------------- */

typedef struct dev_summary {
    tool_ctx *c;
    int devices;
    int plcs;
    int verbose;
    strbuf plc_names;
} dev_summary;

static int summarize_device(void *ctx, th device, const cJSON *item, const char *group_path)
{
    dev_summary *d = ctx;
    d->devices++;
    th cpu = 0;
    th sw = nav_device_plc(device, &cpu);
    char *sw_name = sw ? td_get_s(sw, "Name") : NULL;
    if (sw) {
        d->plcs++;
        sb_printf(&d->plc_names, "%s%s", d->plc_names.len ? ", " : "", tdi_s(item, "Name") ? tdi_s(item, "Name") : "?");
    }
    if (d->verbose) {
        const char *type = tdi_s(item, "TypeIdentifier");
        out(d->c, "%s  [type=%s", tdi_s(item, "Name") ? tdi_s(item, "Name") : "?", type ? type : "?");
        if (sw) {
            char *cpu_name = td_get_s(cpu, "Name");
            char *order = td_get_s(cpu, "TypeIdentifier");
            out(d->c, ", plc=%s", sw_name ? sw_name : "?");
            if (cpu_name && sw_name && strcmp(cpu_name, sw_name) != 0)
                out(d->c, ", cpu=%s", cpu_name);
            if (order)
                out(d->c, ", cpuType=%s", order);
            free(cpu_name);
            free(order);
        }
        if (group_path && *group_path)
            out(d->c, ", group=%s", group_path);
        out(d->c, "]\n");
    }
    free(sw_name);
    return 0;
}

static void project_summary(tool_ctx *c, th project, int list_devices)
{
    cJSON *a = td_attrs(project, "Name,Path,Version,IsModified");
    out(c, "Project: %s\n", tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Name")) ? tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Name")) : "?");
    out(c, "Path: %s\n", tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Path")) ? tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Path")) : "?");
    const char *ver = tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Version"));
    if (ver && *ver)
        out(c, "Version: %s\n", ver);
    out(c, "Unsaved changes: %s\n", tdv_b(cJSON_GetObjectItemCaseSensitive(a, "IsModified"), 0) ? "yes" : "no");
    cJSON_Delete(a);
    dev_summary d = { c, 0, 0, list_devices, { 0 } };
    sb_init(&d.plc_names);
    if (list_devices)
        out(c, "Devices:\n");
    nav_each_device(project, summarize_device, &d);
    if (!list_devices)
        out(c, "Devices: %d (PLCs: %s)\n", d.devices, d.plc_names.len ? sb_str(&d.plc_names) : "none");
    sb_free(&d.plc_names);
}

static void connection_line(tool_ctx *c)
{
    out(c, "Connected to TIA Portal V21 (pid %ld, %s%s)\n", session_pid(), session_with_ui() ? "with UI" : "without UI",
        session_launched() ? ", started by tiaComandante" : "");
}

static int is_project_file(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot || _strnicmp(dot, ".ap", 3) != 0 || !isdigit((unsigned char)dot[3]))
        return 0;
    for (const char *p = dot + 3; *p; p++)
        if (!isdigit((unsigned char)*p))
            return 0;
    return 1;
}

typedef struct proj_list {
    tool_ctx *c;
    int n;
    char first[TC_PATH_MAX];
} proj_list;

static int list_project_file(void *ctx, const char *path, int is_dir, long long size, long long mtime)
{
    (void)size;
    proj_list *l = ctx;
    if (is_dir || !is_project_file(path))
        return 0;
    char when[32];
    fmt_unix_time(mtime, when, sizeof when);
    const char *dot = strrchr(path, '.');
    out(l->c, "%s  [tia=V%s, modified=%s]\n", path, dot + 3, when);
    if (!l->n)
        snprintf(l->first, sizeof l->first, "%s", path);
    l->n++;
    return l->n >= 500;
}

/* projectPath may be the .apXX file or the project folder. */
static int resolve_project_file(tool_ctx *c, const char *in, char *out_path, size_t cap)
{
    char full[TC_PATH_MAX];
    if (fs_full_path(in, full, sizeof full) != 0)
        return fail(c, "invalid projectPath '%s'", in);
    if (fs_is_file(full)) {
        snprintf(out_path, cap, "%s", full);
        return 0;
    }
    if (fs_is_dir(full)) {
        tool_ctx probe;
        ctx_init(&probe, c->tool, NULL);
        proj_list l = { &probe, 0, "" };
        fs_walk(full, NULL, 0, list_project_file, &l);
        ctx_free(&probe);
        if (l.n == 1) {
            snprintf(out_path, cap, "%s", l.first);
            return 0;
        }
        return fail(c, "folder '%s' contains %d TIA project files; pass the .ap21 file", full, l.n);
    }
    return fail(c, "project '%s' not found", full);
}

static int same_path(const char *a, const char *b)
{
    return a && b && _stricmp(a, b) == 0;
}

static int ascii_name(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if ((unsigned char)*s > 126 || *s < 32 || strchr("\\/:*?\"<>|", *s))
            return 0;
    return 1;
}

static int is_modified(th project)
{
    int m = 0;
    return td_get_b(project, "IsModified", &m) == 0 && m;
}

/* Applies saveChanges=prompt|save|discard|auto to the open project.
   Returns 0 to continue, -1 on failure (already reported). */
static int handle_unsaved(tool_ctx *c, th project, const char *policy_default, const char *what)
{
    if (!project || !is_modified(project))
        return 0;
    const char *policy = arg_s(c, "saveChanges");
    if (!policy || !*policy)
        policy = policy_default;
    if (_stricmp(policy, "save") == 0 || _stricmp(policy, "auto") == 0) {
        if (session_check_offline(c) != 0)
            return -1;
        if (td_call_v(project, "Save", NULL) != 0)
            return fail_td(c, "saving the project failed");
        out(c, "Project saved.\n");
        return 0;
    }
    if (_stricmp(policy, "discard") == 0)
        return 0;
    return fail(c, "the project has unsaved changes. Repeat %s with saveChanges=save (save first) or saveChanges=discard.", what);
}

/* ---- actions -------------------------------------------------------------- */

static int a_info(tool_ctx *c)
{
    out(c, "tiaComandante %s - %d tools, %d actions, TIA Portal V21 Openness.\n", TC_VERSION, registry_count(),
        registry_action_total());
    out(c, "Workflow: get_state -> connect (running TIA) | open (project file) | create -> list_devices -> work with "
           "deviceName -> save.\n");
    return 0;
}

static int a_get_state(tool_ctx *c)
{
    int level = 0;
    const char *hint = "";
    if (session_bridge(NULL) != 0) {
        out(c, "State: 0 (TIA Portal V21 Openness unavailable)\n%s\n", session_bridge_error());
        out(c, "Restart required: no (install TIA Portal V21 with Openness, then retry)\n");
        return 0;
    }
    th procs = td_static_h("Siemens.Engineering.TiaPortal", "GetProcesses", NULL);
    cJSON *list = procs ? td_enum(procs, "Id,Mode,ProjectPath", -1) : NULL;
    int nproc = cJSON_GetArraySize(list);
    out(c, "TIA Portal V21 processes: %d\n", nproc);
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        const char *pp = tdi_s(it, "ProjectPath");
        out(c, "  pid=%lld  [mode=%s, project=%s]\n", tdi_i(it, "Id", 0), tdi_s(it, "Mode") ? tdi_s(it, "Mode") : "?",
            pp ? pp : "none");
    }
    cJSON_Delete(list);
    if (nproc > 0) {
        level = 1;
        hint = "session action=connect";
    } else {
        hint = "start TIA Portal V21, or session action=open/launch";
    }
    if (session_portal()) {
        level = 2;
        hint = "session action=open or create";
        connection_line(c);
        th project = session_refresh_project();
        if (project) {
            level = 3;
            hint = "add a PLC to the project in TIA Portal";
            dev_summary d = { c, 0, 0, 0, { 0 } };
            sb_init(&d.plc_names);
            nav_each_device(project, summarize_device, &d);
            char *name = td_get_s(project, "Name");
            out(c, "Project: %s (unsaved changes: %s), devices: %d, PLCs: %s\n", name ? name : "?",
                is_modified(project) ? "yes" : "no", d.devices, d.plc_names.len ? sb_str(&d.plc_names) : "none");
            free(name);
            if (d.plcs > 0) {
                level = 4;
                hint = "ready: use deviceName with the PLC tools";
            }
            sb_free(&d.plc_names);
        }
    } else {
        out(c, "Connection: not connected\n");
    }
    static const char *names[] = { "TIA Portal not running", "TIA Portal running, not connected",
                                   "connected, no project open", "project open, no PLC", "ready" };
    out(c, "State: %d (%s)\nNext: %s\n", level, names[level], hint);
    out(c, "Read-only mode: %s\n", config_read_only() ? "ON" : "off");
    return 0;
}

static int a_connect(tool_ctx *c)
{
    long pid = (long)arg_i(c, "processId", 0);
    if (session_bridge(c) != 0)
        return -1;
    /* A lost connection was already released by session_prepare. */
    if (session_portal() && (!pid || pid == session_pid())) {
        out(c, "Already connected.\n");
        connection_line(c);
        th project = session_refresh_project();
        if (project)
            project_summary(c, project, 0);
        else
            out(c, "No project open. Use session action=open or create.\n");
        return 0;
    }
    tool_ctx probe;
    ctx_init(&probe, c->tool, NULL);
    int rc = session_attach(&probe, pid);
    if (rc != 0 && pid == 0 && strstr(sb_str(&probe.out), "no TIA Portal V21 is running")) {
        ctx_free(&probe);
        out(c, "No TIA Portal V21 running: starting one...\n");
        if (session_launch(c, arg_b(c, "withUI", 1)) != 0)
            return -1;
    } else if (rc != 0) {
        out_raw(c, sb_str(&probe.out));
        c->is_error = 1;
        ctx_free(&probe);
        return -1;
    } else {
        ctx_free(&probe);
    }
    connection_line(c);
    th project = session_refresh_project();
    if (project)
        project_summary(c, project, 0);
    else
        out(c, "No project open. Use session action=open or create.\n");
    return 0;
}

static int a_launch(tool_ctx *c)
{
    if (session_launch(c, arg_b(c, "withUI", 1)) != 0)
        return -1;
    connection_line(c);
    out(c, "No project open. Use session action=open or create.\n");
    return 0;
}

static int a_open(tool_ctx *c)
{
    const char *in = arg_s(c, "projectPath");
    if (!in || !*in) {
        if (!g_cfg.projects_root[0])
            return fail(c, "pass projectPath, or set a projects root with session action=configure projectsRoot=...");
        out(c, "Projects under %s:\n", g_cfg.projects_root);
        proj_list l = { c, 0, "" };
        fs_walk(g_cfg.projects_root, NULL, 3, list_project_file, &l);
        if (!l.n)
            out(c, "(none)\n");
        return 0;
    }
    char path[TC_PATH_MAX];
    if (resolve_project_file(c, in, path, sizeof path) != 0)
        return -1;
    if (session_bridge(c) != 0)
        return -1;

    /* Already open somewhere? Attach to that TIA Portal (idempotent). */
    th procs = td_static_h("Siemens.Engineering.TiaPortal", "GetProcesses", NULL);
    cJSON *list = procs ? td_enum(procs, "Id,ProjectPath", -1) : NULL;
    long owner = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        if (same_path(tdi_s(it, "ProjectPath"), path))
            owner = (long)tdi_i(it, "Id", 0);
    }
    cJSON_Delete(list);
    if (owner) {
        if (session_pid() != owner && session_attach(c, owner) != 0)
            return -1;
        th p = session_refresh_project();
        out(c, "Project already open.\n");
        connection_line(c);
        if (p)
            project_summary(c, p, 0);
        return 0;
    }

    if (session_portal()) {
        th cur = session_refresh_project();
        if (cur) {
            char *name = td_get_s(cur, "Name");
            fail(c, "project '%s' is already open in this TIA Portal. Close it first with session action=close_project.",
                 name ? name : "?");
            free(name);
            return -1;
        }
    } else if (session_ensure_portal(c, arg_b(c, "withUI", 1)) != 0) {
        return -1;
    }

    progress(c, 0, 0, "opening project");
    th project = session_open_project(c, path, arg_b(c, "upgrade", 0));
    if (!project) {
        if (c->is_error)
            return -1; /* already explained (e.g. protected project without credentials) */
        if (strstr(td_err(), "upgrade") || strstr(td_err(), "Upgrade") || strstr(td_err(), "version"))
            return fail(c, "opening %s failed: %s. If the project comes from an older TIA Portal version, retry with upgrade=true "
                           "(the project is converted to V21).",
                        path, td_err());
        return fail_td(c, "opening the project failed");
    }
    td_release(project);
    project = session_refresh_project();
    out(c, "Project opened.\n");
    connection_line(c);
    if (project)
        project_summary(c, project, 0);
    return 0;
}

static int a_create(tool_ctx *c)
{
    const char *name = arg_req(c, "name");
    if (!name)
        return -1;
    if (!ascii_name(name))
        return fail(c, "project name '%s' must be plain ASCII without \\ / : * ? \" < > |", name);
    const char *parent = arg_s(c, "parentDirectory");
    if (!parent || !*parent)
        parent = g_cfg.projects_root;
    if (!*parent)
        return fail(c, "pass parentDirectory or set projectsRoot with session action=configure");
    char full[TC_PATH_MAX];
    if (fs_full_path(parent, full, sizeof full) != 0 || fs_mkdirs(full) != 0)
        return fail(c, "cannot use parent directory '%s'", parent);
    char target[TC_PATH_MAX];
    fs_join(target, sizeof target, full, name);
    if (fs_is_dir(target))
        return fail(c, "'%s' already exists", target);
    if (session_portal()) {
        th cur = session_refresh_project();
        if (cur) {
            char *n = td_get_s(cur, "Name");
            fail(c, "project '%s' is open in this TIA Portal. Close it first with session action=close_project.", n ? n : "?");
            free(n);
            return -1;
        }
    } else if (session_ensure_portal(c, arg_b(c, "withUI", 1)) != 0) {
        return -1;
    }
    progress(c, 0, 0, "creating project");
    th projects = td_get_h(session_portal(), "Projects");
    th project = td_call_h(projects, "Create", tda("Ds", full, name));
    if (!project)
        return fail_td(c, "creating the project failed");
    td_release(project);
    project = session_refresh_project();
    out(c, "Project created.\n");
    connection_line(c);
    if (project)
        project_summary(c, project, 0);
    return 0;
}

static int a_get_project(tool_ctx *c)
{
    th project = session_project();
    cJSON *a = td_attrs(project, "Name,Path,Version,Author,Copyright,Family,CreationTime,LastModified,LastModifiedBy,IsModified,Size");
    static const char *keys[] = { "Name", "Path", "Version", "Author", "Copyright", "Family", "CreationTime",
                                  "LastModified", "LastModifiedBy", "IsModified", "Size" };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, keys[i]);
        if (!v || tdv_is_err(v) || cJSON_IsNull(v))
            continue;
        char *t = tdv_text(v);
        if (strcmp(keys[i], "Size") == 0) {
            char sz[32];
            fmt_size(tdv_i(v, -1), sz, sizeof sz);
            out(c, "%s: %s\n", keys[i], sz);
        } else if (*t) {
            out(c, "%s: %s\n", keys[i], t);
        }
        free(t);
    }
    cJSON_Delete(a);
    dev_summary d = { c, 0, 0, 0, { 0 } };
    sb_init(&d.plc_names);
    nav_each_device(project, summarize_device, &d);
    out(c, "Devices: %d (PLCs: %s)\n", d.devices, d.plc_names.len ? sb_str(&d.plc_names) : "none");
    sb_free(&d.plc_names);
    return 0;
}

static int a_is_dirty(tool_ctx *c)
{
    int m = 0;
    if (td_get_b(session_project(), "IsModified", &m) != 0)
        return fail_td(c, "reading IsModified failed");
    out(c, "%s\n", m ? "true" : "false");
    return 0;
}

static int a_list_devices(tool_ctx *c)
{
    dev_summary d = { c, 0, 0, 1, { 0 } };
    sb_init(&d.plc_names);
    nav_each_device(session_project(), summarize_device, &d);
    if (!d.devices)
        out(c, "(no devices)\n");
    out(c, "Total: %d device(s), %d with PLC software. Use the device name (or the plc name) as deviceName.\n", d.devices,
        d.plcs);
    sb_free(&d.plc_names);
    return 0;
}

static int a_save(tool_ctx *c)
{
    progress(c, 0, 0, "saving project");
    if (td_call_v(session_project(), "Save", NULL) != 0)
        return fail_td(c, "saving the project failed");
    out(c, "Project saved.\n");
    return 0;
}

static int a_save_as(tool_ctx *c)
{
    const char *name = arg_req(c, "newName");
    if (!name)
        return -1;
    if (!ascii_name(name))
        return fail(c, "newName '%s' must be plain ASCII without \\ / : * ? \" < > |", name);
    const char *parent = arg_s(c, "newParentDirectory");
    if (!parent || !*parent)
        parent = g_cfg.projects_root;
    if (!*parent)
        return fail(c, "pass newParentDirectory or set projectsRoot with session action=configure");
    char full[TC_PATH_MAX], target[TC_PATH_MAX];
    if (fs_full_path(parent, full, sizeof full) != 0 || fs_mkdirs(full) != 0)
        return fail(c, "cannot use directory '%s'", parent);
    fs_join(target, sizeof target, full, name);
    if (fs_is_dir(target))
        return fail(c, "'%s' already exists", target);
    progress(c, 0, 0, "saving project copy");
    if (td_call_v(session_project(), "SaveAs", tda("D", target)) != 0)
        return fail_td(c, "SaveAs failed");
    th p = session_refresh_project();
    out(c, "Project saved as %s\n", target);
    if (p)
        project_summary(c, p, 0);
    return 0;
}

static int a_close_project(tool_ctx *c)
{
    th project = session_project();
    if (handle_unsaved(c, project, "prompt", "close_project") != 0)
        return -1;
    char *name = td_get_s(project, "Name");
    if (td_call_v(project, "Close", NULL) != 0) {
        free(name);
        return fail_td(c, "closing the project failed");
    }
    session_refresh_project();
    out(c, "Project '%s' closed.\n", name ? name : "?");
    free(name);
    return 0;
}

static int a_archive(tool_ctx *c)
{
    th project = session_project();
    if (handle_unsaved(c, project, "prompt", "archive") != 0)
        return -1;
    const char *dir = arg_s(c, "outputDirectory");
    if (!dir || !*dir)
        dir = g_cfg.archives_root;
    if (!*dir)
        return fail(c, "archives root not configured: set it with session action=configure archivesRoot=..., or pass "
                       "outputDirectory");
    char full[TC_PATH_MAX];
    if (fs_full_path(dir, full, sizeof full) != 0 || fs_mkdirs(full) != 0)
        return fail(c, "cannot use output directory '%s'", dir);
    char *pname = td_get_s(project, "Name");
    const char *base = arg_s(c, "archiveName");
    char name[512];
    snprintf(name, sizeof name, "%s", base && *base ? base : (pname ? pname : "project"));
    free(pname);
    if (arg_b(c, "timestamp", 1)) {
        char ts[32];
        time_t now = time(NULL);
        struct tm tm;
        localtime_s(&tm, &now);
        strftime(ts, sizeof ts, "_%Y%m%d_%H%M%S", &tm);
        strncat(name, ts, sizeof name - strlen(name) - 1);
    }
    const char *mode = arg_s(c, "archivationMode");
    if (!mode || !*mode)
        mode = "Compressed";
    /* TIA uses targetName as the file name as given: add the extension of a compressed archive */
    if (strstr(mode, "Compressed") && _stricmp(name + (strlen(name) > 6 ? strlen(name) - 6 : 0), ".zap21") != 0)
        strncat(name, ".zap21", sizeof name - strlen(name) - 1);
    progress(c, 0, 0, "archiving project");
    if (td_call_v(project, "Archive", tda("Dse", full, name, "Siemens.Engineering.ProjectArchivationMode", mode)) != 0)
        return fail_td(c, "archiving failed");
    char path[TC_PATH_MAX], sz[32];
    fs_join(path, sizeof path, full, name);
    fmt_size(fs_file_size(path), sz, sizeof sz);
    out(c, "Archive created: %s (%s, mode %s).\n", path, fs_is_file(path) ? sz : "folder", mode);
    return 0;
}

typedef struct arch_list {
    tool_ctx *c;
    int n;
} arch_list;

static int list_archive(void *ctx, const char *path, int is_dir, long long size, long long mtime)
{
    arch_list *l = ctx;
    const char *dot = strrchr(path, '.');
    if (is_dir || !dot || _strnicmp(dot, ".zap", 4) != 0)
        return 0;
    char when[32], sz[32];
    fmt_unix_time(mtime, when, sizeof when);
    fmt_size(size, sz, sizeof sz);
    out(l->c, "%s  [size=%s, date=%s, tia=V%s]\n", fs_basename(path), sz, when, dot[4] ? dot + 4 : "?");
    l->n++;
    return 0;
}

static int a_list_archives(tool_ctx *c)
{
    const char *dir = arg_s(c, "archivesRoot");
    if (!dir || !*dir)
        dir = g_cfg.archives_root;
    if (!*dir)
        return fail(c, "archives root not configured: pass archivesRoot or set it with session action=configure");
    if (!fs_is_dir(dir))
        return fail(c, "folder '%s' does not exist", dir);
    out(c, "Archives in %s:\n", dir);
    arch_list l = { c, 0 };
    fs_walk(dir, NULL, 1, list_archive, &l);
    if (!l.n)
        out(c, "(none)\n");
    return 0;
}

static int set_root(tool_ctx *c, const char *arg, char *dst, size_t cap, int create)
{
    const char *v = arg_s(c, arg);
    if (!v)
        return 0;
    if (!*v) {
        dst[0] = 0;
        return 1;
    }
    char full[TC_PATH_MAX];
    if (fs_full_path(v, full, sizeof full) != 0)
        return fail(c, "invalid path for %s: %s", arg, v);
    if (!fs_is_dir(full)) {
        if (!create || fs_mkdirs(full) != 0)
            return fail(c, "%s: folder '%s' does not exist", arg, full);
    }
    snprintf(dst, cap, "%s", full);
    return 1;
}

static int a_configure(tool_ctx *c)
{
    int changed = 0, rc;
    if ((rc = set_root(c, "projectsRoot", g_cfg.projects_root, sizeof g_cfg.projects_root, 0)) < 0)
        return -1;
    changed += rc;
    if ((rc = set_root(c, "archivesRoot", g_cfg.archives_root, sizeof g_cfg.archives_root, 0)) < 0)
        return -1;
    changed += rc;
    if ((rc = set_root(c, "exportsRoot", g_cfg.exports_root, sizeof g_cfg.exports_root, 1)) < 0)
        return -1;
    changed += rc;
    if ((rc = set_root(c, "librariesRoot", g_cfg.libraries_root, sizeof g_cfg.libraries_root, 0)) < 0)
        return -1;
    changed += rc;
    if (arg_has(c, "httpEnabled"))
        out(c, "Note: httpEnabled is not supported (tiaComandante uses stdio only).\n");
    if (changed && config_save() != 0)
        return fail(c, "cannot write %s", g_cfg.config_file);
    out(c, "%s\n", changed ? "Configuration saved." : "Current configuration:");
    out(c, "projectsRoot: %s\narchivesRoot: %s\nexportsRoot: %s\nlibrariesRoot: %s\nreadOnly: %s\nconfig file: %s\n",
        *g_cfg.projects_root ? g_cfg.projects_root : "(not set)", *g_cfg.archives_root ? g_cfg.archives_root : "(not set)",
        g_cfg.exports_root, *g_cfg.libraries_root ? g_cfg.libraries_root : "(not set)",
        config_read_only() ? "true" : "false", g_cfg.config_file);
    return 0;
}

static int a_disconnect(tool_ctx *c)
{
    if (!session_portal()) {
        out(c, "Not connected.\n");
        return 0;
    }
    int close_tia = arg_b(c, "closeTia", 0);
    th project = session_refresh_project();
    if (project && (close_tia || session_launched())) {
        if (handle_unsaved(c, project, "prompt", "disconnect") != 0)
            return -1;
        if (close_tia && td_call_v(project, "Close", NULL) != 0)
            return fail_td(c, "closing the project failed");
    } else if (project && arg_s(c, "saveChanges") && _stricmp(arg_s(c, "saveChanges"), "save") == 0) {
        if (handle_unsaved(c, project, "save", "disconnect") != 0)
            return -1;
    }
    long pid = session_pid();
    if (close_tia) {
        if (session_close_tia(c) != 0)
            return -1;
        out(c, "Disconnected and TIA Portal (pid %ld) closed.\n", pid);
    } else {
        int launched = session_launched();
        session_release();
        out(c, "Disconnected from TIA Portal (pid %ld)%s.\n", pid, launched ? "; the instance started by tiaComandante was closed" : ", which keeps running");
    }
    return 0;
}

/* ---- table ---------------------------------------------------------------- */

static const action_def actions[] = {
    { "archive", "optional outputDirectory, archiveName, timestamp=true, archivationMode=Compressed, saveChanges",
      "Create a .zap21 archive of the open project. outputDirectory defaults to the configured archives root - do NOT pass "
      "it unless the user asks for a different folder.",
      a_archive, AF_PROJECT | AF_OFFLINE },
    { "close_project", "optional saveChanges=prompt|save|discard|auto",
      "Close the open project. With unsaved changes and saveChanges=prompt the call is refused so you can ask the user.",
      a_close_project, AF_PROJECT },
    { "configure", "optional projectsRoot, archivesRoot, exportsRoot, librariesRoot",
      "Set default folders for create/open/archive/exports/libraries (validated; exportsRoot is created if missing). "
      "Without arguments shows the current configuration.",
      a_configure, 0 },
    { "connect", "optional withUI=true, processId",
      "Attach to the running TIA Portal V21 (preferring the one with a project open) and load its project. Starts TIA Portal "
      "if none is running. Releases any stale handle first.",
      a_connect, AF_BRIDGE },
    { "create", "name; optional parentDirectory, withUI=true",
      "Create a new project. Attaches to a running TIA Portal without project or starts one. ASCII names only. "
      "parentDirectory defaults to the configured projects root.",
      a_create, AF_BRIDGE | AF_WRITES },
    { "disconnect", "optional saveChanges, closeTia=false",
      "Release the Openness connection. closeTia=true also closes the TIA Portal process (asks about unsaved changes "
      "through saveChanges).",
      a_disconnect, 0 },
    { "get_project", "", "Name, path, version, author, dates, size, unsaved changes and device count of the open project.",
      a_get_project, AF_PROJECT },
    { "get_state", "",
      "Proactive state check: state level 0-4 (0 TIA not running/unavailable, 1 running not connected, 2 connected without "
      "project, 3 project without PLC, 4 ready), processes, project and next step. Call at session start.",
      a_get_state, 0 },
    { "info", "", "Server version, tool count and workflow hint.", a_info, 0 },
    { "is_dirty", "", "true/false: the open project has unsaved changes.", a_is_dirty, AF_PROJECT },
    { "launch", "optional withUI=true",
      "Start a new TIA Portal V21 instance (no project). Prefer open/create, which start TIA Portal when needed.", a_launch,
      AF_BRIDGE },
    { "list_archives", "optional archivesRoot", "List .zap* archives (size, date, TIA version) in the archives root.",
      a_list_archives, 0 },
    { "list_devices", "",
      "All devices of the project with type, PLC software name and CPU type. Provides deviceName for the other tools.",
      a_list_devices, AF_PROJECT },
    { "open", "optional projectPath, withUI=true, upgrade=false",
      "Open a project (.ap21 file or its folder), attaching to or starting TIA Portal as needed. Idempotent. Without "
      "projectPath lists the projects in the configured projects root. upgrade=true converts older projects to V21.",
      a_open, AF_BRIDGE },
    { "save", "", "Save the project. OFFLINE REQUIRED: refused while any PLC is online (diagnostics go_offline first).",
      a_save, AF_PROJECT | AF_WRITES | AF_OFFLINE | AF_NO_TX },
    { "save_as", "newName; optional newParentDirectory",
      "Save a copy of the project at a new location (newParentDirectory defaults to the projects root) and continue with "
      "the copy.",
      a_save_as, AF_PROJECT | AF_WRITES | AF_OFFLINE | AF_NO_TX },
};

const tool_def tool_session = {
    .name = "session",
    .title = "Session management",
    .summary = "TIA Portal V21 connection and project lifecycle: state check, attach/start TIA Portal, open/create/save/"
               "archive/close projects, list devices, default folders. Start here.",
    .properties =
        "{"
        "\"withUI\":{\"type\":\"boolean\",\"description\":\"Show the TIA Portal user interface when TIA Portal has to be started (default true).\"},"
        "\"processId\":{\"type\":\"integer\",\"description\":\"connect: attach to this TIA Portal process id (see get_state).\"},"
        "\"projectPath\":{\"type\":\"string\",\"description\":\"open: full path of the .ap21 project file or of its folder.\"},"
        "\"upgrade\":{\"type\":\"boolean\",\"description\":\"open: upgrade a project made with an older TIA Portal version.\"},"
        "\"name\":{\"type\":\"string\",\"description\":\"create: project name (ASCII).\"},"
        "\"parentDirectory\":{\"type\":\"string\",\"description\":\"create: folder that will contain the project folder.\"},"
        "\"newName\":{\"type\":\"string\",\"description\":\"save_as: name of the project copy.\"},"
        "\"newParentDirectory\":{\"type\":\"string\",\"description\":\"save_as: folder for the copy.\"},"
        "\"saveChanges\":{\"type\":\"string\",\"enum\":[\"prompt\",\"save\",\"discard\",\"auto\"],\"description\":\"close_project/archive/disconnect: what to do with unsaved changes (default prompt = refuse and ask).\"},"
        "\"closeTia\":{\"type\":\"boolean\",\"description\":\"disconnect: also close the TIA Portal process.\"},"
        "\"outputDirectory\":{\"type\":\"string\",\"description\":\"archive: target folder (default archives root).\"},"
        "\"archiveName\":{\"type\":\"string\",\"description\":\"archive: archive name without extension (default project name).\"},"
        "\"timestamp\":{\"type\":\"boolean\",\"description\":\"archive: append _YYYYMMDD_HHMMSS (default true).\"},"
        "\"archivationMode\":{\"type\":\"string\",\"enum\":[\"Compressed\",\"None\",\"DiscardRestorableData\",\"DiscardRestorableDataAndCompressed\"],\"description\":\"archive: default Compressed.\"},"
        "\"archivesRoot\":{\"type\":\"string\",\"description\":\"configure / list_archives: archives folder.\"},"
        "\"projectsRoot\":{\"type\":\"string\",\"description\":\"configure: default projects folder.\"},"
        "\"exportsRoot\":{\"type\":\"string\",\"description\":\"configure: default folder for exported files.\"},"
        "\"librariesRoot\":{\"type\":\"string\",\"description\":\"configure: default global libraries folder.\"}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
    .hints = 0,
};
