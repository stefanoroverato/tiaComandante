/* diagnostics: online connection, device IP, network scan, online/offline compare. */
#include "tools.h"

#include "tia/online.h"
#include "tia/session.h"
#include "tia/tia_dyn.h"
#include "tia/tia_nav.h"
#include "tia/tia_sw.h"
#include "util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int a_get_device_ip(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    char ip[64];
    strbuf details;
    sb_init(&details);
    on_device_ip(&plc, ip, sizeof ip, &details);
    if (!ip[0])
        out(c, "%s has no IP address configured on its Ethernet interfaces.\n", plc.device_name);
    else
        out(c, "Project-configured IP of %s: %s (default target for configure_connection / download).\n", plc.device_name, ip);
    if (details.len)
        out(c, "Interfaces:\n%s", sb_str(&details));
    sb_free(&details);
    return 0;
}

static void connection_summary(tool_ctx *c, th provider, const char *label)
{
    th cfg = provider ? td_get_h(provider, "Configuration") : 0;
    int configured = 0;
    if (cfg)
        td_get_b(cfg, "IsConfigured", &configured);
    out(c, "%s connection configured: %s\n", label, configured ? "yes" : "no (use configure_connection)");
    td_clear_err();
}

static int a_get_plc_status(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    th op = on_online_provider(&plc);
    if (!op)
        return fail(c, "%s has no online provider", plc.device_name);
    char *state = td_get_s(op, "State");
    out(c, "Device: %s (PLC %s)\nConnection state: %s\n", plc.device_name, plc.plc_name, state ? state : "?");
    free(state);
    connection_summary(c, op, "Online");
    char ip[64];
    on_device_ip(&plc, ip, sizeof ip, NULL);
    out(c, "Configured IP: %s\n", *ip ? ip : "(none)");
    out(c, "Operating state (RUN/STOP): not available - the Openness engineering API does not expose it and live data "
           "is not part of this server.\n");
    return 0;
}

/* Goes online with the configured connection, or with targetIp (+ pcInterfaceName)
   through GoOnline(ConfigurationAddress) - needed when the IP is set directly at
   the device. *went is set when this call changed the state. */
static int bring_online(tool_ctx *c, nav_plc *plc, th op, int *went)
{
    *went = 0;
    char *cur = td_get_s(op, "State");
    int already = cur && strcmp(cur, "Online") == 0;
    free(cur);
    if (already)
        return 0;
    th cfg = td_get_h(op, "Configuration");
    int configured = 0;
    td_get_b(cfg, "IsConfigured", &configured);
    const char *ip = arg_s(c, "targetIp");
    if (!configured && (!ip || !*ip))
        return fail(c, "the online connection of %s is not configured: run diagnostics action=configure_connection, or "
                       "pass targetIp and pcInterfaceName (required when the IP is set directly at the device)",
                    plc->device_name);
    progress(c, 0, 0, "going online");
    cJSON *st = NULL;
    th addr = 0;
    if (ip && *ip) {
        th ti = on_find_target(c, cfg, arg_s(c, "mode"), arg_s(c, "pcInterfaceName"), arg_s(c, "targetInterface"), NULL);
        th addrs = ti ? td_get_h(ti, "Addresses") : 0;
        addr = addrs ? td_call_h(addrs, "Find", tda("s", ip)) : 0;
        td_clear_err();
        if (!addr && addrs)
            addr = td_call_h(addrs, "Create", tda("s", ip));
        if (!addr)
            return c->is_error ? -1 : fail_td(c, "cannot use targetIp");
    }
    char project_ip[64] = "";
    if (!ip || !*ip)
        on_device_ip(plc, project_ip, sizeof project_ip, NULL);
    int legacy_prev;
    if (on_apply_legacy(c, cfg, "going online", &legacy_prev) != 0)
        return -1;
    on_legit legit;
    on_legitimation_begin(&legit, cfg, ip && *ip ? ip : project_ip);
    st = addr ? td_call(op, "GoOnline", tda("h", addr)) : td_call(op, "GoOnline", NULL);
    int unanswered = on_legitimation_end(c, &legit);
    on_restore_legacy(cfg, legacy_prev);
    if (!st && unanswered)
        return fail(c, "going online failed: the PLC asked for authentication that could not be answered (see above)");
    if (!st) {
        fail_td(c, "going online failed");
        on_legacy_hint(c);
        return -1;
    }
    const char *s = tdv_s(st);
    int ok = s && strcmp(s, "Online") == 0;
    if (!ok)
        fail(c, "%s is not online (state %s). Check the cable/IP, PLCSIM, or the PG/PC interface with scan_devices.",
             plc->device_name, s ? s : "?");
    cJSON_Delete(st);
    if (!ok) {
        on_legacy_hint(c);
        return -1;
    }
    *went = 1;
    return 0;
}

static int a_go_online(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    th op = on_online_provider(&plc);
    if (!op)
        return fail(c, "%s has no online provider", plc.device_name);
    int went = 0;
    if (bring_online(c, &plc, op, &went) != 0)
        return -1;
    out(c, "%s: Online%s\n", plc.device_name, went ? "" : " (already)");
    return 0;
}

static int a_go_offline(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    th op = on_online_provider(&plc);
    if (!op)
        return fail(c, "%s has no online provider", plc.device_name);
    if (td_call_v(op, "GoOffline", NULL) != 0)
        return fail_td(c, "going offline failed");
    char *state = td_get_s(op, "State");
    out(c, "%s: %s\n", plc.device_name, state ? state : "Offline");
    free(state);
    return 0;
}

static int a_configure_connection(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *pc = arg_s(c, "pcInterfaceName");
    if (!arg_b(c, "skipConfirm", 0) && !confirmed(c, "I understand") && !confirmed(c, "yes"))
        return fail(c, "this sets the PG/PC interface and target address used to go online and download for %s. Repeat "
                       "with confirm='yes'.",
                    plc.device_name);
    th op = on_online_provider(&plc);
    th cfg = op ? td_get_h(op, "Configuration") : 0;
    if (!cfg)
        return fail_td(c, "no online configuration");
    th ti = on_find_target(c, cfg, arg_s(c, "mode"), pc, arg_s(c, "targetInterface"), NULL);
    if (!ti)
        return -1;
    char ip[64];
    const char *target_ip = arg_s(c, "targetIp");
    if (!target_ip || !*target_ip) {
        on_device_ip(&plc, ip, sizeof ip, NULL);
        target_ip = ip;
    }
    /* The configuration is the target interface (slot); a different IP is passed
       to go_online / download_to_device as targetIp. */
    cJSON *ok = td_call(cfg, "ApplyConfiguration", tda("h", ti));
    if (!ok)
        return fail_td(c, "applying the configuration failed");
    int applied = cJSON_IsTrue(ok);
    cJSON_Delete(ok);
    if (!applied)
        return fail(c, "TIA Portal refused the configuration");
    char *tname = td_get_s(ti, "Name");
    out(c, "Connection of %s configured: PG/PC interface '%s', target '%s'%s%s.\n", plc.device_name, pc ? pc : "?",
        tname ? tname : "?", *target_ip ? ", address " : "", target_ip);
    free(tname);
    return 0;
}

typedef struct ip_map {
    char ip[64];
    char device[256];
} ip_map;

typedef struct ip_collect {
    ip_map items[256];
    int n;
} ip_collect;

static int collect_ip(void *ctx, th device, const cJSON *item, const char *group)
{
    (void)group;
    ip_collect *k = ctx;
    nav_plc p;
    memset(&p, 0, sizeof p);
    p.software = nav_device_plc(device, &p.cpu);
    if (!p.cpu || k->n >= 256)
        return 0;
    on_device_ip(&p, k->items[k->n].ip, sizeof k->items[k->n].ip, NULL);
    if (k->items[k->n].ip[0]) {
        snprintf(k->items[k->n].device, sizeof k->items[k->n].device, "%s", tdi_s(item, "Name") ? tdi_s(item, "Name") : "?");
        k->n++;
    }
    return 0;
}

static void scan_configuration(tool_ctx *c, th cfg, ip_collect *ips, int *found)
{
    cJSON *modes = td_enum(td_get_h(cfg, "Modes"), "Name", -1);
    const cJSON *m;
    cJSON_ArrayForEach(m, modes)
    {
        cJSON *pcs = td_enum(td_get_h(tdv_h(m), "PcInterfaces"), "Name,Number", -1);
        const cJSON *p;
        cJSON_ArrayForEach(p, pcs)
        {
            const char *pname = tdi_s(p, "Name");
            if (cancelled(c))
                break;
            progress(c, *found, 0, pname);
            cJSON *devs = td_call(tdv_h(p), "GetAccessibleDevices", NULL);
            if (!devs) {
                out(c, "[%s / %s] scan failed: %s\n", tdi_s(m, "Name"), pname ? pname : "?", td_err());
                continue;
            }
            cJSON *list = tdv_h(devs) ? td_enum(tdv_h(devs), "Name,Address", -1) : NULL;
            const cJSON *d;
            cJSON_ArrayForEach(d, list)
            {
                const char *addr = tdi_s(d, "Address");
                const char *in = NULL;
                for (int i = 0; addr && i < ips->n; i++)
                    if (strcmp(ips->items[i].ip, addr) == 0)
                        in = ips->items[i].device;
                out(c, "[%s / %s] %s  [address=%s] %s%s%s\n", tdi_s(m, "Name") ? tdi_s(m, "Name") : "?", pname ? pname : "?",
                    tdi_s(d, "Name") ? tdi_s(d, "Name") : "?", addr ? addr : "?", in ? "[IN PROJECT: " : "[NOT IN THIS PROJECT]",
                    in ? in : "", in ? "]" : "");
                (*found)++;
            }
            cJSON_Delete(list);
            cJSON_Delete(devs);
        }
        cJSON_Delete(pcs);
    }
    cJSON_Delete(modes);
    td_clear_err();
}

static int a_scan_devices(tool_ctx *c)
{
    ip_collect *ips = calloc(1, sizeof *ips);
    if (!ips)
        return fail(c, "out of memory");
    nav_each_device(session_project(), collect_ip, ips);
    int found = 0;
    const char *dev = arg_s(c, "deviceName");
    if (dev && *dev) {
        nav_plc plc;
        if (sw_plc(c, &plc) != 0) {
            free(ips);
            return -1;
        }
        th dp = on_download_provider(&plc);
        th cfg = dp ? td_get_h(dp, "Configuration") : 0;
        if (!cfg) {
            free(ips);
            return fail_td(c, "no download configuration for this device");
        }
        scan_configuration(c, cfg, ips, &found);
    } else {
        th up = td_service(session_project(), "Siemens.Engineering.Upload.StationUploadProvider");
        th cfg = up ? td_get_h(up, "Configuration") : 0;
        if (!cfg) {
            free(ips);
            return fail_td(c, "no station upload configuration available");
        }
        scan_configuration(c, cfg, ips, &found);
    }
    free(ips);
    out(c, "%d accessible device(s). This is a layer-2 discovery on the PG/PC interfaces: devices behind routers are not listed.\n",
        found);
    return 0;
}

static void print_compare(tool_ctx *c, th element, int depth, int include_identical, int *lines)
{
    if (!element || depth > 12 || *lines > 1500)
        return;
    cJSON *a = td_attrs(element, "LeftName,RightName,ComparisonResult,DetailedInformation");
    const char *res = tdv_s(cJSON_GetObjectItemCaseSensitive(a, "ComparisonResult"));
    const char *left = tdv_s(cJSON_GetObjectItemCaseSensitive(a, "LeftName"));
    const char *right = tdv_s(cJSON_GetObjectItemCaseSensitive(a, "RightName"));
    const char *info = tdv_s(cJSON_GetObjectItemCaseSensitive(a, "DetailedInformation"));
    int identical = res && (strcmp(res, "ObjectsIdentical") == 0 || strcmp(res, "FolderContentsIdentical") == 0);
    if (include_identical || !identical || depth == 0) {
        const char *verdict = !res ? "?" : strcmp(res, "LeftMissing") == 0 ? "Only on PLC"
                                         : strcmp(res, "RightMissing") == 0 ? "Only in project"
                                         : identical ? "Identical"
                                                     : "Different";
        out(c, "%*s%s  [%s%s%s]\n", depth * 2, "", left && *left ? left : (right ? right : "?"), verdict,
            info && *info ? ": " : "", info && *info ? info : "");
        (*lines)++;
    }
    cJSON_Delete(a);
    if (identical && !include_identical)
        return;
    cJSON *kids = td_enum(td_get_h(element, "Elements"), NULL, -1);
    const cJSON *k;
    cJSON_ArrayForEach(k, kids)
    print_compare(c, tdv_h(k), depth + 1, include_identical, lines);
    cJSON_Delete(kids);
}

static int a_compare_online_offline(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    th op = on_online_provider(&plc);
    if (!op)
        return fail(c, "%s has no online provider", plc.device_name);
    int went = 0;
    if (bring_online(c, &plc, op, &went) != 0)
        return -1;
    progress(c, 0, 0, "comparing with the PLC");
    th res = td_call_h(plc.software, "CompareToOnline", NULL);
    int rc = 0;
    if (!res) {
        rc = fail_td(c, "online/offline comparison failed");
    } else {
        int lines = 0;
        print_compare(c, td_get_h(res, "RootElement"), 0, arg_b(c, "includeIdentical", 0), &lines);
        if (lines > 1500)
            out(c, "(truncated)\n");
    }
    if (went) {
        td_call_v(op, "GoOffline", NULL);
        td_clear_err();
        out(c, "(went online for the comparison and back offline)\n");
    }
    return rc;
}

static const action_def actions[] = {
    { "compare_online_offline", "deviceName; optional includeIdentical=false, targetIp, pcInterfaceName, legacyCommunication",
      "Compare the offline project software (blocks, tags, types, technology objects) with the PLC. Read-only. Goes "
      "online automatically if needed (and back offline). Per item: Identical / Different / Only on PLC / Only in project.",
      a_compare_online_offline, AF_PROJECT },
    { "configure_connection", "deviceName, pcInterfaceName, confirm; optional targetIp, targetInterface, mode=PN/IE, skipConfirm=false",
      "Set the PG/PC interface and target address used to go online/download. targetIp defaults to the project-configured "
      "IP - omit it normally; pass it only to reach a different address (e.g. from scan_devices). Without a valid "
      "pcInterfaceName the available interfaces are listed.",
      a_configure_connection, AF_PROJECT | AF_WRITES },
    { "get_device_ip", "deviceName",
      "Read-only: the project-configured IP of the PLC plus every Ethernet interface with IP, mask, router and subnet.",
      a_get_device_ip, AF_PROJECT },
    { "get_plc_status", "deviceName",
      "Connection state (Offline/Online/NotReachable/Protected/...), whether the online connection is configured and the "
      "configured IP. The CPU operating state (RUN/STOP) is not available through Openness.",
      a_get_plc_status, AF_PROJECT },
    { "go_offline", "deviceName", "Disconnect from the PLC.", a_go_offline, AF_PROJECT },
    { "go_online", "deviceName; optional targetIp, pcInterfaceName, legacyCommunication",
      "Connect to the PLC through the configured connection (run download_upload action=download_check first). targetIp "
      "(with pcInterfaceName) reaches a different address than the project IP. A protected PLC gets its password / PLC "
      "user from the Windows Credential Manager (admin action=set_credential kind=plc key=<PLC IP>).",
      a_go_online,
      AF_PROJECT },
    { "scan_devices", "optional deviceName",
      "Read-only network scan (GetAccessibleDevices) on all PG/PC interfaces: name and address of each accessible device, "
      "annotated [IN PROJECT: device] or [NOT IN THIS PROJECT]. With deviceName the device's download configuration is "
      "used, otherwise the project's station upload configuration.",
      a_scan_devices, AF_PROJECT },
};

const tool_def tool_diagnostics = {
    .name = "diagnostics",
    .title = "PLC diagnostics & connection",
    .summary = "Online connection management and checks: device IP, connection configuration, go online/offline, network "
               "scan, online/offline comparison, connection status.",
    .properties =
        "{"
        "\"deviceName\":{\"type\":\"string\",\"description\":\"Device or PLC name (session list_devices).\"},"
        "\"pcInterfaceName\":{\"type\":\"string\",\"description\":\"PG/PC interface, e.g. 'PLCSIM' or the network adapter name.\"},"
        "\"targetIp\":{\"type\":\"string\"},\"targetInterface\":{\"type\":\"string\"},"
        "\"mode\":{\"type\":\"string\",\"description\":\"Connection mode, default PN/IE.\"},"
        "\"confirm\":{\"type\":\"string\"},\"skipConfirm\":{\"type\":\"boolean\"},"
        "\"includeIdentical\":{\"type\":\"boolean\"},"
        "\"legacyCommunication\":{\"type\":\"boolean\",\"description\":\"go_online, compare_online_offline: use legacy "
        "(non-secure) PG/PC communication, if the CPU allows it.\"}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
};
