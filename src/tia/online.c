#include "online.h"

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
