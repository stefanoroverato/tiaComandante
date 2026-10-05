#include "online.h"

#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

th on_online_provider(const nav_plc *plc)
{
    return plc->cpu ? td_service(plc->cpu, "Siemens.Engineering.Online.OnlineProvider") : 0;
}

th on_download_provider(const nav_plc *plc)
{
    return plc->cpu ? td_service(plc->cpu, "Siemens.Engineering.Download.DownloadProvider") : 0;
}

typedef struct ip_ctx {
    char *ip;
    size_t cap;
    strbuf *details;
} ip_ctx;

static void node_details(ip_ctx *k, const char *iface, th node)
{
    char *addr = td_get_s(node, "Address");
    char *mask = td_get_s(node, "SubnetMask");
    char *router = td_get_s(node, "RouterAddress");
    char *pnname = td_get_s(node, "PnDeviceName");
    char *ipmode = td_get_s(node, "IpProtocolSelection");
    int use_router = 0;
    td_get_b(node, "UseRouter", &use_router);
    th subnet = td_get_h(node, "ConnectedSubnet");
    char *sname = subnet ? td_get_s(subnet, "Name") : NULL;
    td_clear_err();
    if (addr && *addr && !k->ip[0])
        snprintf(k->ip, k->cap, "%s", addr);
    if (k->details)
        sb_printf(k->details, "%s  [ip=%s, mask=%s, router=%s, ipMode=%s, subnet=%s%s%s]\n", iface,
                  addr && *addr ? addr : "(not set in the project)", mask && *mask ? mask : "?",
                  use_router && router ? router : "none", ipmode ? ipmode : "?", sname ? sname : "not connected",
                  pnname && *pnname ? ", pnName=" : "", pnname && *pnname ? pnname : "");
    free(ipmode);
    free(addr);
    free(mask);
    free(router);
    free(pnname);
    free(sname);
}

static int ip_item(void *ctx, th item, const cJSON *it, int depth)
{
    (void)depth;
    ip_ctx *k = ctx;
    th ni = td_service(item, "Siemens.Engineering.HW.Features.NetworkInterface");
    if (!ni)
        return 0;
    cJSON *nodes = td_enum(td_get_h(ni, "Nodes"), "Name,NodeType", -1);
    const cJSON *n;
    cJSON_ArrayForEach(n, nodes)
    {
        const char *type = tdi_s(n, "NodeType");
        if (type && strcmp(type, "Ethernet") != 0)
            continue;
        node_details(k, tdi_s(it, "Name") ? tdi_s(it, "Name") : "?", tdv_h(n));
    }
    cJSON_Delete(nodes);
    td_clear_err();
    return 0;
}

void on_device_ip(const nav_plc *plc, char *ip, size_t cap, strbuf *details)
{
    ip[0] = 0;
    ip_ctx k = { ip, cap, details };
    if (plc->cpu)
        nav_each_device_item(plc->cpu, 2, ip_item, &k);
}

void on_describe_interfaces(tool_ctx *c, th configuration)
{
    cJSON *modes = td_enum(td_get_h(configuration, "Modes"), "Name", -1);
    const cJSON *m;
    cJSON_ArrayForEach(m, modes)
    {
        out(c, "Mode %s:\n", tdi_s(m, "Name") ? tdi_s(m, "Name") : "?");
        cJSON *pcs = td_enum(td_get_h(tdv_h(m), "PcInterfaces"), "Name,Number", -1);
        const cJSON *p;
        cJSON_ArrayForEach(p, pcs)
        {
            out(c, "  PG/PC interface '%s' (#%lld) -> targets:", tdi_s(p, "Name") ? tdi_s(p, "Name") : "?", tdi_i(p, "Number", 0));
            cJSON *tis = td_enum(td_get_h(tdv_h(p), "TargetInterfaces"), "Name", -1);
            const cJSON *t;
            cJSON_ArrayForEach(t, tis)
            out(c, " '%s'", tdi_s(t, "Name") ? tdi_s(t, "Name") : "?");
            cJSON_Delete(tis);
            out(c, "\n");
        }
        cJSON_Delete(pcs);
    }
    cJSON_Delete(modes);
    td_clear_err();
}

static int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; hay && *hay; hay++)
        if (_strnicmp(hay, needle, n) == 0)
            return 1;
    return 0;
}

th on_find_target(tool_ctx *c, th configuration, const char *mode, const char *pc_interface, const char *target,
                  th *pc_out)
{
    if (pc_out)
        *pc_out = 0;
    const char *want_mode = mode && *mode ? mode : "PN/IE";
    cJSON *modes = td_enum(td_get_h(configuration, "Modes"), "Name", -1);
    th mode_h = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, modes)
    {
        if (tdi_s(m, "Name") && _stricmp(tdi_s(m, "Name"), want_mode) == 0)
            mode_h = tdv_h(m);
    }
    cJSON_Delete(modes);
    th pc = 0;
    if (mode_h && pc_interface && *pc_interface) {
        cJSON *pcs = td_enum(td_get_h(mode_h, "PcInterfaces"), "Name", -1);
        const cJSON *p;
        cJSON_ArrayForEach(p, pcs)
        {
            const char *n = tdi_s(p, "Name");
            if (n && _stricmp(n, pc_interface) == 0) {
                pc = tdv_h(p);
                break;
            }
            if (!pc && n && contains_ci(n, pc_interface))
                pc = tdv_h(p);
        }
        cJSON_Delete(pcs);
    }
    if (pc_out)
        *pc_out = pc; /* also on failure: station upload works on the PG/PC interface itself */
    th ti = 0;
    if (pc) {
        cJSON *tis = td_enum(td_get_h(pc, "TargetInterfaces"), "Name", -1);
        const cJSON *t;
        cJSON_ArrayForEach(t, tis)
        {
            const char *n = tdi_s(t, "Name");
            if (!ti || (target && *target && n && contains_ci(n, target)))
                ti = tdv_h(t);
        }
        cJSON_Delete(tis);
    }
    if (!ti) {
        fail(c, "%s", !mode_h ? "connection mode not found"
                     : !pc_interface || !*pc_interface ? "pass pcInterfaceName (see the list below)"
                     : !pc ? "PG/PC interface not found"
                           : "no target interface on that PG/PC interface");
        on_describe_interfaces(c, configuration);
        return 0;
    }
    return ti;
}

int on_apply_legacy(tool_ctx *c, th configuration, const char *what, int *prev)
{
    *prev = -1;
    if (!arg_has(c, "legacyCommunication"))
        return 0;
    int legacy = arg_b(c, "legacyCommunication", 0), old = 0;
    if (td_get_b(configuration, "EnableLegacyCommunication", &old) != 0)
        return fail_td(c, "cannot read EnableLegacyCommunication");
    if (old != legacy) {
        if (td_set(configuration, "EnableLegacyCommunication", cJSON_CreateBool(legacy)) != 0)
            return fail_td(c, "cannot set EnableLegacyCommunication");
        *prev = old;
    }
    out(c, "Legacy (non-secure) PG/PC communication %s for %s.\n", legacy ? "enabled" : "disabled", what);
    return 0;
}

void on_restore_legacy(th configuration, int prev)
{
    if (prev < 0)
        return;
    char etype[128], emsg[1024];
    snprintf(etype, sizeof etype, "%s", td_err_type());
    snprintf(emsg, sizeof emsg, "%s", td_err());
    if (td_set(configuration, "EnableLegacyCommunication", cJSON_CreateBool(prev)) != 0)
        LOG_W("restoring EnableLegacyCommunication: %s", td_err());
    if (*etype || *emsg)
        td_set_err(etype, "%s", emsg);
    else
        td_clear_err();
}

void on_legacy_hint(tool_ctx *c)
{
    if (!arg_b(c, "legacyCommunication", 0))
        out(c, "Hint: if the CPU accepts it, legacyCommunication=true uses non-secure PG/PC communication (needed e.g. "
               "when the project does not hold the PLC certificate yet).\n");
}

/* ---- PLC passwords ----------------------------------------------------------------------- */

void on_secret_init(on_secret *s, const char *ip)
{
    memset(s, 0, sizeof *s);
    snprintf(s->ip, sizeof s->ip, "%s", ip ? ip : "");
}

void on_secret_free(on_secret *s)
{
    cred_free(&s->cr);
    memset(s, 0, sizeof *s);
}

static int secret_get(on_secret *s)
{
    if (!s->looked_up) {
        s->looked_up = 1;
        s->found = cred_get(CRED_PLC, s->ip, &s->cr) == 0 && s->cr.password;
    }
    return s->found;
}

/* User types accepted by the PLC, e.g. "PasswordOnly" or "ProjectUser, GlobalUser". */
static void supported_types(th cfg, char *buf, size_t cap)
{
    buf[0] = 0;
    th list = td_call_h(cfg, "GetSupportedAuthenticationTypes", NULL);
    cJSON *items = list ? td_enum(list, "CurrentUserType", -1) : NULL;
    size_t n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        const char *t = tdi_s(it, "CurrentUserType");
        if (t && n < cap)
            n += (size_t)snprintf(buf + n, cap - n, "%s%s", n ? ", " : "", t);
    }
    cJSON_Delete(items);
    td_clear_err();
}

int on_answer_password(on_secret *s, th cfg, const char **note)
{
    static const char *const simple[] = {
        "Siemens.Engineering.Download.Configurations.ModuleReadAccessPassword",
        "Siemens.Engineering.Download.Configurations.ModuleWriteAccessPassword",
        "Siemens.Engineering.Upload.Configurations.UploadPasswordConfiguration",
        "Siemens.Engineering.Online.Configurations.OnlinePasswordConfiguration",
        NULL,
    };
    *note = NULL;
    if (!cfg)
        return -1;
    int simple_pw = 0, auth = 0;
    for (int i = 0; simple[i] && !simple_pw; i++)
        simple_pw = td_is(cfg, simple[i]);
    if (!simple_pw)
        auth = td_is(cfg, "Siemens.Engineering.Online.Configurations.OnlineAuthenticationConfiguration");
    if (!simple_pw && !auth) {
        int other = td_is(cfg, "Siemens.Engineering.Download.Configurations.DownloadPasswordConfiguration");
        td_clear_err();
        if (other) /* block binding / master secret passwords are not PLC access passwords */
            *note = "NOT HANDLED - this secret is not stored by tiaComandante";
        return other ? 1 : -1;
    }
    if (!secret_get(s)) {
        *note = "NOT HANDLED - no PLC credential stored (admin action=set_credential kind=plc key=<PLC IP>)";
        return 1;
    }
    int rc;
    if (simple_pw) {
        rc = td_call_v(cfg, "SetPassword", tda("s", s->cr.password));
        *note = "password from the Windows Credential Manager";
    } else {
        /* PLC user management, or an access-level password only: user name "-"
           (or none), or a PLC that lists no user types (an S7-1500 with access
           levels reports only AnonymousUser and accepts PasswordOnly). */
        char types[160] = "";
        supported_types(cfg, types, sizeof types);
        int user_types = strstr(types, "ProjectUser") || strstr(types, "GlobalUser");
        int password_only = !s->cr.user[0] || strcmp(s->cr.user, "-") == 0 || (*types && !user_types);
        const char *type = password_only ? "PasswordOnly" : s->cr.global ? "GlobalUser" : "ProjectUser";
        th oc = td_get_h(cfg, "OnlineCredentials");
        rc = !oc || td_set(oc, "Type", cJSON_CreateString(type)) != 0 ||
             (!password_only && td_set(oc, "Name", cJSON_CreateString(s->cr.user)) != 0) ||
             td_call_v(oc, "SetPassword", tda("s", s->cr.password)) != 0;
        snprintf(s->note, sizeof s->note, "%s from the Windows Credential Manager (%s; PLC supports: %s)",
                 password_only ? "PLC password" : "PLC user credentials", type, *types ? types : "?");
        *note = s->note;
    }
    if (rc != 0) {
        LOG_W("answering a PLC password request failed: %s", td_err());
        *note = "NOT HANDLED - setting the stored password failed";
    }
    td_clear_err();
    return rc != 0;
}

/* TIA Portal keeps calling OnlineLegitimation handlers after they are removed,
   e.g. during a later download: one permanent callback answers only while an
   online call is in progress (g_legit) and ignores the requests otherwise. */
static long long g_legit_cb;
static on_legit *volatile g_legit;

static int on_legitimation(void *ctx, const cJSON *args, cJSON **result)
{
    (void)ctx;
    (void)result;
    on_legit *l = g_legit;
    if (!l) {
        LOG_D("OnlineLegitimation outside an online call: ignored");
        return 0;
    }
    th cfg = tdv_h(cJSON_GetArrayItem(args, cJSON_GetArraySize(args) - 1));
    char *full = cfg ? td_typename(cfg) : NULL;
    const char *type = full ? (strrchr(full, '.') ? strrchr(full, '.') + 1 : full) : "?";
    const char *note = NULL;
    int rc = on_answer_password(&l->secret, cfg, &note);
    if (rc < 0) {
        if (cfg && td_is(cfg, "Siemens.Engineering.Online.Configurations.TlsVerificationConfiguration"))
            note = "NOT HANDLED - trust the PLC certificate in TIA Portal (or use legacyCommunication)";
        else
            note = "NOT HANDLED";
    }
    if (rc != 0)
        l->secret.unanswered++;
    td_clear_err();
    sb_printf(&l->log, "  %s: %s\n", type, note);
    free(full);
    return 0;
}

void on_legitimation_begin(on_legit *l, th cfg, const char *ip)
{
    memset(l, 0, sizeof *l);
    on_secret_init(&l->secret, ip);
    sb_init(&l->log);
    if (!g_legit_cb)
        g_legit_cb = td_register_callback(on_legitimation, NULL);
    g_legit = l;
    l->sub = cfg ? td_subscribe(cfg, "OnlineLegitimation", g_legit_cb) : 0;
    if (!l->sub)
        LOG_W("OnlineLegitimation not available: %s", td_err());
    td_clear_err();
}

int on_legitimation_end(tool_ctx *c, on_legit *l)
{
    g_legit = NULL;
    int unanswered = l->secret.unanswered;
    if (l->sub)
        td_unsubscribe(l->sub);
    if (l->log.len)
        out(c, "Online legitimation requests:\n%s", sb_str(&l->log));
    sb_free(&l->log);
    on_secret_free(&l->secret);
    return unanswered;
}
