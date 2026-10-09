#include "session.h"

#include "app/config.h"
#include "app/credentials.h"
#include "tia/tia_env.h"
#include "tia/tia_nav.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/strbuf.h"
#include "util/utf.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T_TIA_PORTAL "Siemens.Engineering.TiaPortal"

static struct {
    int bridge_tried;
    int bridge_ok;
    char bridge_err[512];

    th portal;
    th project;
    long pid;
    int launched;
    int with_ui;

    long long cb_confirm, cb_notify, cb_disposed;
    long long sub_confirm, sub_notify, sub_disposed;
    volatile LONG disposed;
    volatile LONG in_call; /* tool calls running (session_call_begin/end) */

    SRWLOCK ev_lock;
    strbuf events;
} S = { .ev_lock = SRWLOCK_INIT };

/* ---- bridge ------------------------------------------------------------- */

int session_bridge(tool_ctx *c)
{
    if (!S.bridge_tried) {
        S.bridge_tried = 1;
        wchar_t openness[MAX_PATH];
        char exe[TC_PATH_MAX];
        if (tia_find_openness_dir(openness, MAX_PATH) != 0) {
            snprintf(S.bridge_err, sizeof S.bridge_err,
                     "TIA Portal V21 Openness not found (PublicAPI\\V21\\net48). Install TIA Portal V21 with "
                     "the Openness option or set TIACMD_OPENNESS_DIR.");
        } else if (fs_exe_dir(exe, sizeof exe) == 0) {
            char dll[TC_PATH_MAX];
            fs_join(dll, sizeof dll, exe, "TiaComandante.Bridge.dll");
            wchar_t *wdll = utf8_to_wide(dll);
            char err[512];
            if (td_init(wdll, openness, err, sizeof err) == 0) {
                S.bridge_ok = 1;
                char *o = wide_to_utf8(openness);
                LOG_I(".NET bridge started, Openness assemblies in %s", o ? o : "?");
                free(o);
            } else {
                snprintf(S.bridge_err, sizeof S.bridge_err, "cannot start the .NET bridge: %s", err);
            }
            free(wdll);
        }
        if (!S.bridge_ok)
            LOG_E("%s", S.bridge_err);
    }
    if (!S.bridge_ok)
        return c ? fail(c, "%s", S.bridge_err) : -1;
    return 0;
}

int session_bridge_ok(void) { return S.bridge_ok; }
const char *session_bridge_error(void) { return S.bridge_err; }

/* ---- events --------------------------------------------------------------- */

static void add_event(const char *kind, const char *caption, const char *text)
{
    AcquireSRWLockExclusive(&S.ev_lock);
    if (S.events.len < 16384)
        sb_printf(&S.events, "[TIA %s] %s%s%s\n", kind, caption ? caption : "", caption && *caption && text && *text ? ": " : "",
                  text ? text : "");
    ReleaseSRWLockExclusive(&S.ev_lock);
    LOG_I("TIA %s: %s / %s", kind, caption ? caption : "", text ? text : "");
}

/* choices is a flags enum as text, e.g. "Yes, No, Cancel". */
static int has_choice(const char *choices, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = choices; p && *p;) {
        while (*p == ' ' || *p == ',')
            p++;
        size_t len = strcspn(p, ", ");
        if (len == n && _strnicmp(p, name, n) == 0)
            return 1;
        p += len;
    }
    return 0;
}

/* Answer for the configured confirmations policy, or NULL to leave the dialog to TIA. */
static const char *pick_answer(const char *choices)
{
    if (InterlockedCompareExchange(&S.in_call, 0, 0) == 0)
        return NULL; /* raised by the user working in TIA Portal, not by a tool call */
    static const char *const negative[] = { "Cancel", "No", "NoToAll", "Abort", "Ok", NULL };
    static const char *const positive[] = { "Yes", "YesToAll", "Ok", NULL };
    const char *const *order = g_cfg.confirmations == CONF_CANCEL ? negative
                               : g_cfg.confirmations == CONF_ACCEPT ? positive
                                                                    : NULL;
    for (int i = 0; order && order[i]; i++)
        if (has_choice(choices, order[i]))
            return order[i];
    return NULL;
}

static int on_confirmation(void *ctx, const cJSON *args, cJSON **result)
{
    (void)ctx;
    (void)result;
    if (S.in_call)
        LOG_W("TIA confirmation event during a tool call (policy %s)", config_confirmation_names[g_cfg.confirmations % 3]);
    else
        LOG_I("TIA confirmation event outside a tool call");
    th e = tdv_h(cJSON_GetArrayItem(args, 1));
    char *caption = td_get_s(e, "Caption");
    char *text = td_get_s(e, "Text");
    char *choices = td_get_s(e, "Choices");
    const char *answer = pick_answer(choices);
    if (answer && (td_set(e, "Result", cJSON_CreateString(answer)) != 0 || td_set(e, "IsHandled", cJSON_CreateTrue()) != 0)) {
        LOG_W("cannot answer TIA confirmation: %s", td_err());
        answer = NULL;
    }
    td_clear_err();
    char buf[2048];
    snprintf(buf, sizeof buf, "%s (choices: %s; %s%s)", text ? text : "", choices ? choices : "?",
             answer ? "answered " : S.in_call ? "left to TIA Portal" : "outside a tool call, left to the TIA Portal user",
             answer ? answer : "");
    add_event("confirmation", caption, buf);
    free(caption);
    free(text);
    free(choices);
    return 0;
}

static int on_notification(void *ctx, const cJSON *args, cJSON **result)
{
    (void)ctx;
    (void)result;
    th e = tdv_h(cJSON_GetArrayItem(args, 1));
    char *caption = td_get_s(e, "Caption");
    char *text = td_get_s(e, "Text");
    add_event("notification", caption, text);
    free(caption);
    free(text);
    return 0;
}

static int on_disposed(void *ctx, const cJSON *args, cJSON **result)
{
    (void)ctx;
    (void)args;
    (void)result;
    InterlockedExchange(&S.disposed, 1);
    LOG_W("TIA Portal session disposed");
    return 0;
}

/* ---- connection --------------------------------------------------------- */

th session_portal(void) { return S.portal; }
th session_project(void) { return S.project; }
long session_pid(void) { return S.pid; }
int session_launched(void) { return S.launched; }
int session_with_ui(void) { return S.with_ui; }

static int process_alive(long pid)
{
    if (pid <= 0)
        return 1;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h)
        return 0;
    DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
}

void session_release(void)
{
    nav_cache_clear();
    if (S.sub_confirm)
        td_unsubscribe(S.sub_confirm);
    if (S.sub_notify)
        td_unsubscribe(S.sub_notify);
    if (S.sub_disposed)
        td_unsubscribe(S.sub_disposed);
    S.sub_confirm = S.sub_notify = S.sub_disposed = 0;
    if (S.project) {
        td_release(S.project);
        S.project = 0;
    }
    if (S.portal) {
        if (!S.disposed && process_alive(S.pid) && td_call_v(S.portal, "Dispose", NULL) != 0)
            LOG_W("TiaPortal.Dispose failed: %s", td_err());
        td_release(S.portal);
        S.portal = 0;
    }
    S.pid = 0;
    S.launched = 0;
    InterlockedExchange(&S.disposed, 0);
}

static void adopt(th portal, int launched, int with_ui)
{
    S.portal = portal;
    td_pin(portal);
    S.launched = launched;
    S.with_ui = with_ui;
    InterlockedExchange(&S.disposed, 0);
    S.pid = 0;
    th proc = td_call_h(portal, "GetCurrentProcess", NULL);
    if (proc) {
        long long id = 0;
        if (td_get_i(proc, "Id", &id) == 0)
            S.pid = (long)id;
        char *mode = td_get_s(proc, "Mode");
        if (mode)
            S.with_ui = strcmp(mode, "WithUserInterface") == 0;
        free(mode);
    }
    if (!S.cb_confirm) {
        S.cb_confirm = td_register_callback(on_confirmation, NULL);
        S.cb_notify = td_register_callback(on_notification, NULL);
        S.cb_disposed = td_register_callback(on_disposed, NULL);
    }
    S.sub_confirm = td_subscribe(portal, "Confirmation", S.cb_confirm);
    S.sub_notify = td_subscribe(portal, "Notification", S.cb_notify);
    S.sub_disposed = td_subscribe(portal, "Disposed", S.cb_disposed);
    td_clear_err();
    session_refresh_project();
    LOG_I("TIA Portal %s (pid %ld, %s)", launched ? "launched" : "attached", S.pid, S.with_ui ? "with UI" : "without UI");
}

int session_attach(tool_ctx *c, long pid)
{
    if (session_bridge(c) != 0)
        return -1;
    th procs = td_static_h(T_TIA_PORTAL, "GetProcesses", NULL);
    if (!procs)
        return fail_td(c, "TiaPortal.GetProcesses failed");
    cJSON *list = td_enum(procs, "Id,ProjectPath", -1);
    th chosen = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        long id = (long)tdi_i(it, "Id", 0);
        if (pid > 0) {
            if (id == pid) {
                chosen = tdv_h(it);
                break;
            }
        } else if (!chosen || tdi_s(it, "ProjectPath")) {
            chosen = tdv_h(it);
            if (tdi_s(it, "ProjectPath"))
                break;
        }
    }
    int n = cJSON_GetArraySize(list);
    cJSON_Delete(list);
    if (!chosen) {
        if (pid > 0)
            return fail(c, "no TIA Portal V21 process with id %ld", pid);
        return fail(c, "no TIA Portal V21 is running (%d processes). Start TIA Portal V21 or use session action=launch/open.", n);
    }
    if (S.portal)
        session_release();
    th portal = td_call_h(chosen, "Attach", NULL);
    if (!portal)
        return fail_td(c, "attach to TIA Portal failed (check that your Windows user is in the 'Siemens TIA Openness' group and that access was allowed in TIA Portal)");
    adopt(portal, 0, 1);
    return 0;
}

int session_launch(tool_ctx *c, int with_ui)
{
    if (session_bridge(c) != 0)
        return -1;
    if (S.portal)
        session_release();
    progress(c, 0, 0, "starting TIA Portal V21");
    th portal = td_new(T_TIA_PORTAL, tda("s", with_ui ? "WithUserInterface" : "WithoutUserInterface"));
    if (!portal)
        return fail_td(c, "starting TIA Portal failed");
    adopt(portal, 1, with_ui);
    return 0;
}

int session_ensure_portal(tool_ctx *c, int with_ui)
{
    if (S.portal)
        return 0;
    if (session_bridge(c) != 0)
        return -1;
    /* Prefer a running TIA Portal without a project; otherwise launch a new one. */
    th procs = td_static_h(T_TIA_PORTAL, "GetProcesses", NULL);
    cJSON *list = procs ? td_enum(procs, "Id,ProjectPath", -1) : NULL;
    long free_pid = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        if (!tdi_s(it, "ProjectPath")) {
            free_pid = (long)tdi_i(it, "Id", 0);
            break;
        }
    }
    cJSON_Delete(list);
    if (free_pid)
        return session_attach(c, free_pid);
    return session_launch(c, with_ui);
}

typedef struct umac_ctx {
    cred cr;
    int answered;
} umac_ctx;

static int on_umac(void *ctx, const cJSON *args, cJSON **result)
{
    (void)result;
    umac_ctx *u = ctx;
    th credentials = tdv_h(cJSON_GetArrayItem(args, 0));
    if (!credentials)
        return 1;
    td_set(credentials, "Name", cJSON_CreateString(u->cr.user));
    td_set(credentials, "Type", cJSON_CreateString(u->cr.global ? "Global" : "Project"));
    td_call_v(credentials, "SetPassword", tda("s", u->cr.password));
    if (td_failed())
        LOG_W("UMAC credentials: %s", td_err());
    u->answered++;
    return 0;
}

static int is_protection_error(const char *e)
{
    return strstr(e, "protected") || strstr(e, "not authorized") || strstr(e, "access authorization") ||
           strstr(e, "user management") || strstr(e, "Umac");
}

th session_open_project(tool_ctx *c, const char *path, int upgrade)
{
    th projects = S.portal ? td_get_h(S.portal, "Projects") : 0;
    if (!projects) {
        fail_td(c, "cannot access the project list");
        return 0;
    }
    const char *method = upgrade ? "OpenWithUpgrade" : "Open";
    th project = td_call_h(projects, method, tda("f", path));
    if (project)
        return project;
    if (strstr(td_err(), "already been opened")) {
        fail_td(c, "the project is locked");
        out(c, "It is open in another TIA Portal instance: use session action=get_state to find it and session action=connect "
               "to work there. After a crash TIA Portal releases the lock about 2 minutes later.\n");
        return 0;
    }
    if (!is_protection_error(td_err()))
        return 0; /* the caller reports td_err() */

    umac_ctx u;
    memset(&u, 0, sizeof u);
    if (cred_get(CRED_UMAC, path, &u.cr) != 0) {
        fail(c, "the project is protected by user management (UMAC) and no credentials are stored for it. Ask the user to "
                "enter them with admin action=set_credential kind=umac key=\"%s\" (a Windows credential dialog opens on "
                "this PC; the password never passes through the chat), then retry.",
             path);
        return 0;
    }
    LOG_I("opening protected project as UMAC user %s (%s)", u.cr.user, u.cr.global ? "Global" : "Project");
    long long cb = td_register_callback(on_umac, &u);
    project = td_call_h(projects, method, tda("fc", path, cb));
    td_unregister_callback(cb);
    if (!project) {
        char err[1024];
        snprintf(err, sizeof err, "%s", td_err());
        fail(c, "opening the protected project as '%s' failed: %s. Check the stored credentials (admin action=set_credential "
                "kind=umac) and the user type (Project/Global).",
             u.cr.user, err);
    } else {
        out(c, "Opened with user management credentials of '%s'.\n", u.cr.user);
    }
    cred_free(&u.cr);
    return project;
}

th session_refresh_project(void)
{
    if (!S.portal)
        return 0;
    th projects = td_get_h(S.portal, "Projects");
    cJSON *list = projects ? td_enum(projects, NULL, 1) : NULL;
    th p = list ? tdv_h(cJSON_GetArrayItem(list, 0)) : 0;
    cJSON_Delete(list);
    if (p != S.project) {
        if (S.project)
            td_release(S.project);
        S.project = p;
        if (p)
            td_pin(p);
    }
    return p;
}

int session_close_tia(tool_ctx *c)
{
    long pid = S.pid;
    int launched = S.launched;
    session_release(); /* disposing a portal we launched also closes it */
    if (launched || pid <= 0 || !process_alive(pid))
        return 0;
    th procs = td_static_h(T_TIA_PORTAL, "GetProcesses", NULL);
    cJSON *list = procs ? td_enum(procs, "Id", -1) : NULL;
    int rc = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        if ((long)tdi_i(it, "Id", 0) == pid) {
            if (td_call_v(tdv_h(it), "Dispose", NULL) != 0)
                rc = fail_td(c, "closing TIA Portal failed");
            break;
        }
    }
    cJSON_Delete(list);
    return rc;
}

typedef struct online_ctx {
    strbuf names;
} online_ctx;

static int check_device_online(void *ctx, th device, const cJSON *item, const char *group_path)
{
    (void)group_path;
    online_ctx *o = ctx;
    th cpu = 0;
    if (!nav_device_plc(device, &cpu) || !cpu)
        return 0;
    char state[64];
    nav_online_state(cpu, state, sizeof state);
    if (strcmp(state, "Online") == 0 || strcmp(state, "Connecting") == 0)
        sb_printf(&o->names, "%s%s", o->names.len ? ", " : "", tdi_s(item, "Name") ? tdi_s(item, "Name") : "?");
    return 0;
}

int session_check_offline(tool_ctx *c)
{
    if (!S.project)
        return 0;
    online_ctx o;
    sb_init(&o.names);
    nav_each_device(S.project, check_device_online, &o);
    int rc = 0;
    if (o.names.len)
        rc = fail(c, "OnlineModeRestriction: device(s) online: %s. Run diagnostics action=go_offline first.", sb_str(&o.names));
    sb_free(&o.names);
    return rc;
}

static int connection_lost(void)
{
    return S.portal && (S.disposed || !process_alive(S.pid));
}

int session_prepare(tool_ctx *c, unsigned flags)
{
    if ((flags & AF_WRITES) && config_read_only())
        return fail(c, "the server runs in read-only mode (readOnly in config.json, --read-only or TIACMD_READONLY): "
                       "action '%s' modifies the project or the PLC", c->action ? c->action->name : c->tool->name);
    if (!(flags & (AF_BRIDGE | AF_PORTAL | AF_PROJECT)))
        return 0;
    if (session_bridge(c) != 0)
        return -1;
    if (connection_lost()) {
        LOG_W("TIA Portal connection lost (pid %ld)", S.pid);
        session_release();
        out(c, "Note: the previous TIA Portal connection was lost and has been released.\n");
    }
    if (!(flags & (AF_PORTAL | AF_PROJECT)))
        return 0;
    if (!S.portal && (flags & AF_WRITES))
        return fail(c, "not connected to TIA Portal. Actions that modify a project never auto-connect: choose the target "
                       "explicitly with session action=connect, open or create first.");
    if (!S.portal) {
        tool_ctx probe;
        ctx_init(&probe, c->tool, NULL);
        int rc = session_attach(&probe, 0);
        if (rc == 0)
            out(c, "(auto-connected to TIA Portal V21, pid %ld)\n", S.pid);
        ctx_free(&probe);
        if (rc != 0)
            return fail(c, "not connected to TIA Portal and no running TIA Portal V21 to attach to. "
                           "Use session action=connect, open or launch.");
    }
    if (flags & AF_PROJECT) {
        if (!session_refresh_project())
            return fail(c, "no project is open in TIA Portal. Use session action=open or create.");
        if ((flags & AF_OFFLINE) && session_check_offline(c) != 0)
            return -1;
    }
    return 0;
}

static session_guard *g_guard; /* guard of the running call, if any */

static void guard_open_tx(session_guard *g)
{
    g->tx = td_call_h(g->access, "Transaction", tda("hs", S.project, g->label));
    if (!g->tx) {
        LOG_W("no transaction for %s: %s", g->label, td_err());
        td_clear_err();
    }
}

int session_guard_begin(tool_ctx *c, unsigned flags, session_guard *g)
{
    memset(g, 0, sizeof *g);
    if ((flags & (AF_PROJECT | AF_WRITES)) != (AF_PROJECT | AF_WRITES) || !g_cfg.exclusive_access || !S.portal || !S.project)
        return 0;
    snprintf(g->label, sizeof g->label, "tiaComandante: %s%s%s", c->tool->name, c->action ? "." : "",
             c->action ? c->action->name : "");
    g->access = td_call_h(S.portal, "ExclusiveAccess", tda("s", g->label));
    if (!g->access)
        return fail_td(c, "TIA Portal refused exclusive access (a dialog is open, or another Openness client holds it); "
                          "close it and retry, or set \"exclusiveAccess\": false in config.json");
    if (g_cfg.transactions && !(flags & AF_NO_TX))
        guard_open_tx(g);
    g_guard = g;
    return 0;
}

/* Commits (or, if TIA refuses, rolls back) and closes the open transaction.
   Returns 0 when committed. */
static int guard_close_tx(session_guard *g, int commit)
{
    int ok = 0;
    if (commit) {
        int can = 1;
        td_get_b(g->tx, "CanCommit", &can);
        ok = can && td_call_v(g->tx, "CommitOnDispose", NULL) == 0;
        if (!ok)
            LOG_W("%s: TIA Portal refused the commit: %s", g->label, td_err());
    }
    if (td_call_v(g->tx, "Dispose", NULL) != 0) {
        LOG_W("%s: closing the transaction: %s", g->label, td_err());
        ok = 0;
    }
    g->tx = 0;
    td_clear_err();
    return ok ? 0 : -1;
}

th session_compile(th compilable)
{
    session_guard *g = g_guard;
    int split = g && g->tx;
    if (split) {
        if (g->poisoned) {
            guard_close_tx(g, 0); /* the call is repeated anyway: keep nothing */
        } else if (guard_close_tx(g, 1) == 0) {
            g->splits++;
        } else {
            g->poisoned = 1; /* the work so far was rolled back: the call is repeated without a transaction */
            nav_cache_clear();
        }
    }
    th result = td_call_h(compilable, "Compile", NULL);
    if (split) {
        char etype[128], emsg[2048];
        snprintf(etype, sizeof etype, "%s", td_err_type());
        snprintf(emsg, sizeof emsg, "%s", td_err());
        guard_open_tx(g);
        if (!result)
            td_set_err(etype, "%s", emsg);
    }
    return result;
}

int session_guard_end(tool_ctx *c, session_guard *g)
{
    if (!g->access)
        return 0;
    /* Keep the error of the action for session_finish_call. */
    char etype[128], emsg[2048];
    snprintf(etype, sizeof etype, "%s", td_err_type());
    snprintf(emsg, sizeof emsg, "%s", td_err());
    int cancel = 0;
    td_get_b(g->access, "IsCancellationRequested", &cancel);
    if (cancel && !c->is_error)
        fail(c, "cancelled in TIA Portal (exclusive access dialog)");
    int rerun = 0;
    if (g->tx || g->poisoned) {
        int want = !c->is_error && !g->poisoned;
        int committed = g->tx && guard_close_tx(g, want) == 0 && want;
        int refused = want && !committed;
        const char *rolled = g->splits ? "Rolled back the changes made after the last compile of this call.\n"
                                       : "Rolled back: no change of this call was kept in the project.\n";
        if ((g->poisoned || refused) && !cancel && !g->splits) {
            rerun = 1; /* nothing of this call was kept */
        } else if (refused || (g->poisoned && !c->is_error)) {
            fail(c, "TIA Portal refused to commit the changes of this call (an error occurred inside its transaction)");
            out(c, "%s", rolled);
        } else if (c->is_error) {
            out(c, "%s", rolled);
        }
        if (!committed)
            nav_cache_clear();
    }
    g_guard = NULL;
    if (td_call_v(g->access, "Dispose", NULL) != 0)
        LOG_W("exclusive access dispose: %s", td_err());
    if (*etype || *emsg)
        td_set_err(etype, "%s", emsg);
    else
        td_clear_err();
    memset(g, 0, sizeof *g);
    return rerun;
}

void session_call_begin(void)
{
    InterlockedIncrement(&S.in_call);
}

void session_call_end(void)
{
    InterlockedDecrement(&S.in_call);
}

void session_finish_call(tool_ctx *c)
{
    AcquireSRWLockExclusive(&S.ev_lock);
    if (S.events.len) {
        if (c) {
            if (c->out.len && c->out.p[c->out.len - 1] != '\n')
                sb_appendc(&c->out, '\n');
            sb_append(&c->out, sb_str(&S.events));
        }
        sb_clear(&S.events);
    }
    ReleaseSRWLockExclusive(&S.ev_lock);
    if (c && c->is_error) {
        const char *t = td_err_type();
        if (strstr(t, "Disposed") || strstr(t, "invalid or released handle"))
            nav_cache_clear();
        if (strstr(t, "NonRecoverable") || strstr(t, "EngineeringObjectDisposed") || connection_lost()) {
            LOG_W("releasing TIA Portal session after %s", *t ? t : "connection loss");
            session_release();
            out(c, "The TIA Portal connection was lost; reconnect with session action=connect.\n");
        }
    }
}

void session_shutdown(void)
{
    if (S.bridge_ok && S.portal)
        session_release();
}
