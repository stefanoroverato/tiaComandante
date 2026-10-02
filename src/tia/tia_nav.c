#include "tia_nav.h"

#include "util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T_SOFTWARE_CONTAINER "Siemens.Engineering.HW.Features.SoftwareContainer"
#define T_PLC_SOFTWARE "Siemens.Engineering.SW.PlcSoftware"
#define T_ONLINE_PROVIDER "Siemens.Engineering.Online.OnlineProvider"

static int each_in_group(th group, const char *path, nav_device_fn fn, void *ctx, int depth)
{
    int stop = 0;
    cJSON *devs = td_enum(td_get_h(group, "Devices"), "Name,TypeIdentifier", -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, devs)
    {
        if ((stop = fn(ctx, tdv_h(it), it, path)) != 0)
            break;
    }
    cJSON_Delete(devs);
    if (stop || depth > 16)
        return stop;
    cJSON *groups = td_enum(td_get_h(group, "Groups"), "Name", -1);
    cJSON_ArrayForEach(it, groups)
    {
        char sub[1024];
        const char *name = tdi_s(it, "Name");
        snprintf(sub, sizeof sub, "%s%s%s", path, *path ? "/" : "", name ? name : "?");
        if ((stop = each_in_group(tdv_h(it), sub, fn, ctx, depth + 1)) != 0)
            break;
    }
    cJSON_Delete(groups);
    return stop;
}

int nav_each_device(th project, nav_device_fn fn, void *ctx)
{
    int stop = 0;
    cJSON *devs = td_enum(td_get_h(project, "Devices"), "Name,TypeIdentifier", -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, devs)
    {
        if ((stop = fn(ctx, tdv_h(it), it, "")) != 0)
            break;
    }
    cJSON_Delete(devs);
    if (stop)
        return stop;
    cJSON *groups = td_enum(td_get_h(project, "DeviceGroups"), "Name", -1);
    cJSON_ArrayForEach(it, groups)
    {
        const char *name = tdi_s(it, "Name");
        if ((stop = each_in_group(tdv_h(it), name ? name : "?", fn, ctx, 1)) != 0)
            break;
    }
    cJSON_Delete(groups);
    return stop;
}

static int each_item(th parent, int depth, int max_depth, nav_item_fn fn, void *ctx)
{
    int stop = 0;
    cJSON *items = td_enum(td_get_h(parent, "DeviceItems"), "Name", -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        th h = tdv_h(it);
        if ((stop = fn(ctx, h, it, depth)) != 0)
            break;
        if (depth < max_depth && (stop = each_item(h, depth + 1, max_depth, fn, ctx)) != 0)
            break;
    }
    cJSON_Delete(items);
    return stop;
}

int nav_each_device_item(th device, int max_depth, nav_item_fn fn, void *ctx)
{
    return each_item(device, 0, max_depth, fn, ctx);
}

th nav_plc_software(th device_item)
{
    th sc = td_service(device_item, T_SOFTWARE_CONTAINER);
    if (!sc)
        return 0;
    th sw = td_get_h(sc, "Software");
    return td_is(sw, T_PLC_SOFTWARE) ? sw : 0;
}

typedef struct plc_search {
    th software;
    th cpu;
} plc_search;

static int find_sw(void *ctx, th item_h, const cJSON *item, int depth)
{
    (void)item;
    (void)depth;
    plc_search *s = ctx;
    th sw = nav_plc_software(item_h);
    if (sw) {
        s->software = sw;
        s->cpu = item_h;
        return 1;
    }
    return 0;
}

th nav_device_plc(th device, th *cpu_item)
{
    plc_search s = { 0, 0 };
    nav_each_device_item(device, 3, find_sw, &s);
    if (cpu_item)
        *cpu_item = s.cpu;
    return s.software;
}

typedef struct find_ctx {
    const char *wanted;
    nav_plc *out;
    int found;
    strbuf available;
} find_ctx;

static int match_device(void *ctx, th device, const cJSON *item, const char *group_path)
{
    (void)group_path;
    find_ctx *f = ctx;
    th cpu = 0;
    th sw = nav_device_plc(device, &cpu);
    if (!sw)
        return 0;
    const char *dname = tdi_s(item, "Name");
    char *cpu_name = td_get_s(cpu, "Name");
    char *sw_name = td_get_s(sw, "Name");
    sb_printf(&f->available, "%s%s", f->available.len ? ", " : "", dname ? dname : "?");
    if (cpu_name && (!dname || _stricmp(cpu_name, dname) != 0))
        sb_printf(&f->available, " (PLC '%s')", cpu_name);
    int hit = (dname && _stricmp(dname, f->wanted) == 0) || (cpu_name && _stricmp(cpu_name, f->wanted) == 0) ||
              (sw_name && _stricmp(sw_name, f->wanted) == 0);
    if (hit) {
        f->out->device = device;
        f->out->cpu = cpu;
        f->out->software = sw;
        snprintf(f->out->device_name, sizeof f->out->device_name, "%s", dname ? dname : "");
        snprintf(f->out->plc_name, sizeof f->out->plc_name, "%s", sw_name ? sw_name : (cpu_name ? cpu_name : ""));
        f->found = 1;
    }
    free(cpu_name);
    free(sw_name);
    return hit;
}

#define PLC_CACHE 16
static struct {
    char key[256];
    nav_plc plc;
} g_cache[PLC_CACHE];
static int g_ncache;
static th g_cache_project;

void nav_cache_clear(void)
{
    for (int i = 0; i < g_ncache; i++) {
        td_release(g_cache[i].plc.device);
        td_release(g_cache[i].plc.cpu);
        td_release(g_cache[i].plc.software);
    }
    g_ncache = 0;
    g_cache_project = 0;
}

int nav_find_plc(tool_ctx *c, th project, const char *device_name, nav_plc *out)
{
    memset(out, 0, sizeof *out);
    if (project != g_cache_project) {
        nav_cache_clear();
        g_cache_project = project;
    }
    for (int i = 0; i < g_ncache; i++) {
        if (_stricmp(g_cache[i].key, device_name) == 0) {
            *out = g_cache[i].plc;
            return 0;
        }
    }
    find_ctx f = { device_name, out, 0, { 0 } };
    sb_init(&f.available);
    nav_each_device(project, match_device, &f);
    int rc = 0;
    if (!f.found) {
        rc = fail(c, "device '%s' not found or has no PLC software. Available PLCs: %s", device_name,
                  f.available.len ? sb_str(&f.available) : "(none)");
    } else if (g_ncache < PLC_CACHE) {
        td_pin(out->device);
        td_pin(out->cpu);
        td_pin(out->software);
        snprintf(g_cache[g_ncache].key, sizeof g_cache[g_ncache].key, "%s", device_name);
        g_cache[g_ncache].plc = *out;
        g_ncache++;
    }
    sb_free(&f.available);
    return rc;
}

void nav_online_state(th cpu, char *out, size_t cap)
{
    out[0] = 0;
    th op = td_service(cpu, T_ONLINE_PROVIDER);
    if (!op)
        return;
    char *s = td_get_s(op, "State");
    snprintf(out, cap, "%s", s ? s : "");
    free(s);
}
