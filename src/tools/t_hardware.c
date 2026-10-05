/* hardware: device configuration, rack/slot topology, networks, I/O addresses,
   hardware compile, CSV / CAx exports, network settings, hardware catalog. */
#include "tools.h"

#include "app/config.h"
#include "app/devcatalog.h"
#include "tia/session.h"
#include "tia/simaticml.h"
#include "tia/tagaddr.h"
#include "tia/tia_dyn.h"
#include "tia/tia_env.h"
#include "tia/tia_nav.h"
#include "tia/tia_sw.h"
#include "util/fs.h"
#include "util/strbuf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define T_DEVICE "Siemens.Engineering.HW.Device"
#define T_DEVICE_ITEM "Siemens.Engineering.HW.DeviceItem"
#define T_NETIF "Siemens.Engineering.HW.Features.NetworkInterface"
#define ITEM_ATTRS "Name,PositionNumber,Classification"
#define MAX_LINES 3000

static void copy_attr(const cJSON *attrs, const char *name, char *out, size_t cap)
{
    const char *s = tdv_s(cJSON_GetObjectItemCaseSensitive(attrs, name));
    snprintf(out, cap, "%s", s ? s : "");
}

/* ---- device lookup ---------------------------------------------------------------------- */

typedef struct dev_ref {
    th device;
    char name[256];
    char group[512];
} dev_ref;

typedef struct dev_find {
    const char *wanted;
    int pass; /* 0: device names, 1: names of the device items (CPU, head module) */
    dev_ref *out;
    int found;
    strbuf names;
} dev_find;

static int item_named(void *ctx, th h, const cJSON *it, int depth)
{
    (void)h;
    (void)depth;
    const char *n = tdi_s(it, "Name");
    return n && _stricmp(n, ((dev_find *)ctx)->wanted) == 0;
}

static int match_device(void *ctx, th device, const cJSON *item, const char *group)
{
    dev_find *f = ctx;
    const char *n = tdi_s(item, "Name");
    if (f->pass == 0 && f->names.len < 4000)
        sb_printf(&f->names, "%s%s", f->names.len ? ", " : "", n ? n : "?");
    int hit = f->pass == 0 ? n && _stricmp(n, f->wanted) == 0 : nav_each_device_item(device, 1, item_named, f) != 0;
    if (!hit)
        return 0;
    f->out->device = device;
    snprintf(f->out->name, sizeof f->out->name, "%s", n ? n : "?");
    snprintf(f->out->group, sizeof f->out->group, "%s", group ? group : "");
    f->found = 1;
    return 1;
}

/* Resolves deviceName: a device (station) name, or the name of one of its
   items such as the CPU or the interface module. */
static int find_device_named(tool_ctx *c, const char *wanted, dev_ref *out)
{
    memset(out, 0, sizeof *out);
    dev_find f = { wanted, 0, out, 0, { 0 } };
    sb_init(&f.names);
    nav_each_device(session_project(), match_device, &f);
    if (!f.found) {
        f.pass = 1;
        nav_each_device(session_project(), match_device, &f);
    }
    td_clear_err();
    if (!f.found)
        fail(c, "device '%s' not found. Devices: %s", wanted, f.names.len ? sb_str(&f.names) : "(none)");
    else
        snprintf(c->error_context, sizeof c->error_context, "device=%s", out->name);
    sb_free(&f.names);
    return f.found ? 0 : -1;
}

static int find_device(tool_ctx *c, dev_ref *out)
{
    const char *wanted = arg_req(c, "deviceName");
    return wanted ? find_device_named(c, wanted, out) : -1;
}

/* "Device / CPU or head module / interface" for a node, IO connector or IO controller. */
static void owner_text(th obj, char *out, size_t cap)
{
    char dev[256] = "", head[256] = "", iface[256] = "";
    th cur = obj;
    for (int i = 0; i < 12 && cur; i++) {
        th p = td_get_h(cur, "Parent");
        if (!p)
            break;
        if (td_is(p, T_DEVICE)) {
            char *n = td_get_s(p, "Name");
            snprintf(dev, sizeof dev, "%s", n ? n : "?");
            free(n);
            break;
        }
        if (td_is(p, T_DEVICE_ITEM)) {
            char *n = td_get_s(p, "Name");
            if (!iface[0]) {
                snprintf(iface, sizeof iface, "%s", n ? n : "?");
            } else if (!head[0]) {
                char *cls = td_get_s(p, "Classification");
                if (cls && (strcmp(cls, "CPU") == 0 || strcmp(cls, "HM") == 0))
                    snprintf(head, sizeof head, "%s", n ? n : "?");
                free(cls);
            }
            free(n);
        }
        cur = p;
    }
    td_clear_err();
    snprintf(out, cap, "%s%s%s%s%s", dev[0] ? dev : "?", head[0] ? " / " : "", head, iface[0] ? " / " : "", iface);
}

/* ---- device items --------------------------------------------------------------------------- */

typedef struct hw_addr {
    char io; /* I or Q */
    long start;
    int bits;
} hw_addr;

typedef struct hw_item {
    th h;
    char name[256];
    int pos;
    char cls[16]; /* CPU, HM or "" */
    char type_name[128];
    char order[64];
    char fw[32];
    char comment[256];
    hw_addr addr[16];
    int naddr;
    int di, dq, ai, aq, other_ch;
    char other_type[32]; /* Type of the other channels, e.g. Technology */
} hw_item;

static void read_item(th h, const cJSON *it, hw_item *o)
{
    memset(o, 0, sizeof *o);
    o->h = h;
    snprintf(o->name, sizeof o->name, "%s", tdi_s(it, "Name") ? tdi_s(it, "Name") : "?");
    o->pos = (int)tdi_i(it, "PositionNumber", -1);
    const char *cls = tdi_s(it, "Classification");
    snprintf(o->cls, sizeof o->cls, "%s", cls && strcmp(cls, "None") != 0 ? cls : "");
    cJSON *a = td_attrs(h, "TypeName,OrderNumber,FirmwareVersion,Comment");
    copy_attr(a, "TypeName", o->type_name, sizeof o->type_name);
    copy_attr(a, "OrderNumber", o->order, sizeof o->order);
    copy_attr(a, "FirmwareVersion", o->fw, sizeof o->fw);
    copy_attr(a, "Comment", o->comment, sizeof o->comment);
    cJSON_Delete(a);
    cJSON *addrs = td_enum(td_get_h(h, "Addresses"), "StartAddress,Length,IoType", -1);
    const cJSON *x;
    cJSON_ArrayForEach(x, addrs)
    {
        const char *io = tdi_s(x, "IoType");
        char k = io && strcmp(io, "Input") == 0 ? 'I' : io && strcmp(io, "Output") == 0 ? 'Q' : 0;
        if (k && o->naddr < 16) {
            hw_addr *d = &o->addr[o->naddr++];
            d->io = k;
            d->start = (long)tdi_i(x, "StartAddress", 0);
            d->bits = (int)tdi_i(x, "Length", 0);
        }
    }
    cJSON_Delete(addrs);
    cJSON *chs = td_enum(td_get_h(h, "Channels"), "IoType,Type", -1);
    cJSON_ArrayForEach(x, chs)
    {
        const char *io = tdi_s(x, "IoType");
        const char *type = tdi_s(x, "Type");
        int in = io && strcmp(io, "Input") == 0, outp = io && strcmp(io, "Output") == 0;
        int dig = type && strcmp(type, "Digital") == 0, ana = type && strcmp(type, "Analog") == 0;
        if (dig && in)
            o->di++;
        else if (dig && outp)
            o->dq++;
        else if (ana && in)
            o->ai++;
        else if (ana && outp)
            o->aq++;
        else if (o->other_ch++ == 0)
            snprintf(o->other_type, sizeof o->other_type, "%s", type ? type : "other");
    }
    cJSON_Delete(chs);
    td_clear_err();
    if (o->cls[0] && o->order[0])
        dc_profile_seen(o->cls, o->type_name, o->order, o->fw);
}

static void merge_item(hw_item *dst, const hw_item *src)
{
    for (int i = 0; i < src->naddr && dst->naddr < 16; i++)
        dst->addr[dst->naddr++] = src->addr[i];
    dst->di += src->di;
    dst->dq += src->dq;
    dst->ai += src->ai;
    dst->aq += src->aq;
    if (!dst->other_ch)
        snprintf(dst->other_type, sizeof dst->other_type, "%s", src->other_type);
    dst->other_ch += src->other_ch;
}

static void fmt_addr(const hw_addr *a, char *out, size_t cap)
{
    if (a->bits > 0 && a->bits % 8 == 0)
        snprintf(out, cap, "%c %ld..%ld", a->io, a->start, a->start + a->bits / 8 - 1);
    else if (a->bits > 0)
        snprintf(out, cap, "%c %ld.0..%ld.%d", a->io, a->start, a->start + (a->bits - 1) / 8, (a->bits - 1) % 8);
    else
        snprintf(out, cap, "%c %ld", a->io, a->start);
}

static void fmt_channels(const hw_item *o, char *out, size_t cap)
{
    out[0] = 0;
    size_t n = 0;
    const struct {
        int v;
        const char *k;
    } ch[] = { { o->di, "DI" }, { o->dq, "DQ" }, { o->ai, "AI" }, { o->aq, "AQ" },
                 { o->other_ch, o->other_type[0] ? o->other_type : "other" } };
    for (size_t i = 0; i < sizeof ch / sizeof ch[0]; i++)
        if (ch[i].v && n < cap)
            n += (size_t)snprintf(out + n, cap - n, "%s%d %s", n ? " + " : "", ch[i].v, ch[i].k);
}

/* "slot=2, type=..., order=..., fw=..., I 0..1, 16 DI" */
static void item_details(const hw_item *o, strbuf *sb, int with_comment)
{
    sb_printf(sb, "slot=%d", o->pos);
    if (o->cls[0])
        sb_printf(sb, ", class=%s", o->cls);
    if (o->type_name[0] && strcmp(o->type_name, o->name) != 0)
        sb_printf(sb, ", type=%s", o->type_name);
    if (o->order[0])
        sb_printf(sb, ", order=%s", o->order);
    if (o->fw[0])
        sb_printf(sb, ", fw=%s", o->fw);
    for (int i = 0; i < o->naddr; i++) {
        char a[64];
        fmt_addr(&o->addr[i], a, sizeof a);
        sb_printf(sb, ", %s", a);
    }
    char ch[96];
    fmt_channels(o, ch, sizeof ch);
    if (*ch)
        sb_printf(sb, ", channels=%s", ch);
    if (with_comment && o->comment[0])
        sb_printf(sb, ", comment=%s", o->comment);
}

/* Appends the network nodes, IO controller and IO device roles of an item;
   returns the number of nodes. */
static int net_details(th item, strbuf *sb)
{
    th ni = td_service(item, T_NETIF);
    if (!ni) {
        td_clear_err();
        return 0;
    }
    int n = 0;
    cJSON *nodes = td_enum(td_get_h(ni, "Nodes"), "Name,NodeType,Address,SubnetMask,PnDeviceName,ConnectedSubnet", -1);
    const cJSON *x;
    cJSON_ArrayForEach(x, nodes)
    {
        const char *addr = tdi_s(x, "Address");
        const char *mask = tdi_s(x, "SubnetMask");
        const char *pn = tdi_s(x, "PnDeviceName");
        th sub = tdv_h(tdi_a(x, "ConnectedSubnet"));
        char *sname = sub ? td_get_s(sub, "Name") : NULL;
        sb_printf(sb, "%s%s (%s): address=%s%s%s, subnet=%s%s%s", sb->len ? ", " : "", tdi_s(x, "Name") ? tdi_s(x, "Name") : "?",
                  tdi_s(x, "NodeType") ? tdi_s(x, "NodeType") : "?", addr && *addr ? addr : "(not set)", mask && *mask ? "/" : "",
                  mask && *mask ? mask : "", sname ? sname : "not connected", pn && *pn ? ", pnName=" : "", pn && *pn ? pn : "");
        free(sname);
        n++;
    }
    cJSON_Delete(nodes);
    cJSON *ctl = td_enum(td_get_h(ni, "IoControllers"), NULL, -1);
    cJSON_ArrayForEach(x, ctl)
    {
        th ios = td_get_h(tdv_h(x), "IoSystem");
        char *name = ios ? td_get_s(ios, "Name") : NULL;
        long long num = 0;
        if (ios)
            td_get_i(ios, "Number", &num);
        sb_printf(sb, "%sIO controller%s%s%s", sb->len ? ", " : "", name ? " of '" : " (no IO system)", name ? name : "",
                  name ? "'" : "");
        if (name)
            sb_printf(sb, " (%lld)", num);
        free(name);
    }
    cJSON_Delete(ctl);
    cJSON *con = td_enum(td_get_h(ni, "IoConnectors"), NULL, -1);
    int assigned = 0;
    cJSON_ArrayForEach(x, con)
    {
        th ios = td_get_h(tdv_h(x), "ConnectedToIoSystem");
        char *name = ios ? td_get_s(ios, "Name") : NULL;
        if (name)
            sb_printf(sb, "%sIO device of '%s'", sb->len ? ", " : "", name);
        assigned += name != NULL;
        free(name);
    }
    if (cJSON_GetArraySize(con) && !assigned)
        sb_printf(sb, "%sIO device (not assigned to an IO system)", sb->len ? ", " : "");
    cJSON_Delete(con);
    td_clear_err();
    return n;
}

/* Depth-first walk of the items of a device. Children named like their parent
   (the built-in submodule carrying the addresses) are folded into the parent. */
typedef struct tree_walk tree_walk;
struct tree_walk {
    int max_depth;
    int compact; /* skip items without order number, addresses, network nodes or class */
    void (*emit)(tree_walk *w, int depth, const hw_item *o, const char *net, const char *plc);
    void *ctx;
    int lines;
};

static void walk_level(tree_walk *w, th parent, int depth, const char *parent_name)
{
    cJSON *items = td_enum(td_get_h(parent, "DeviceItems"), ITEM_ATTRS, -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        if (w->lines >= MAX_LINES)
            break;
        const char *n = tdi_s(it, "Name");
        if (parent_name && n && strcmp(n, parent_name) == 0)
            continue; /* folded into the parent */
        hw_item *o = malloc(sizeof *o);
        if (!o)
            break;
        read_item(tdv_h(it), it, o);
        cJSON *kids = td_enum(td_get_h(o->h, "DeviceItems"), ITEM_ATTRS, -1);
        const cJSON *k;
        cJSON_ArrayForEach(k, kids)
        {
            const char *kn = tdi_s(k, "Name");
            if (kn && strcmp(kn, o->name) == 0) {
                hw_item sub;
                read_item(tdv_h(k), k, &sub);
                merge_item(o, &sub);
            }
        }
        int has_kids = cJSON_GetArraySize(kids) > 0;
        cJSON_Delete(kids);
        strbuf net;
        sb_init(&net);
        int nodes = net_details(o->h, &net);
        char plc[256] = "";
        if (strcmp(o->cls, "CPU") == 0) {
            th sw = nav_plc_software(o->h);
            char *pn = sw ? td_get_s(sw, "Name") : NULL;
            snprintf(plc, sizeof plc, "%s", pn ? pn : "");
            free(pn);
            td_clear_err();
        }
        if (!w->compact || o->order[0] || o->naddr || nodes || o->cls[0]) {
            w->emit(w, depth, o, sb_str(&net), plc);
            w->lines++;
        }
        sb_free(&net);
        if (has_kids && depth < w->max_depth)
            walk_level(w, o->h, depth + 1, o->name);
        free(o);
    }
    cJSON_Delete(items);
}

typedef struct print_ctx {
    tool_ctx *c;
    int indent;
} print_ctx;

static void emit_line(tree_walk *w, int depth, const hw_item *o, const char *net, const char *plc)
{
    print_ctx *p = w->ctx;
    strbuf d;
    sb_init(&d);
    item_details(o, &d, 1);
    if (*plc)
        sb_printf(&d, ", plc=%s", plc);
    if (*net)
        sb_printf(&d, ", %s", net);
    out(p->c, "%*s%s  [%s]\n", p->indent + depth * 2, "", o->name, sb_str(&d));
    sb_free(&d);
}

static void device_header(tool_ctx *c, th device, const char *name, const char *group)
{
    cJSON *a = td_attrs(device, "TypeIdentifier,IsGsd,Comment");
    const char *type = tdv_s(cJSON_GetObjectItemCaseSensitive(a, "TypeIdentifier"));
    const char *cm = tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Comment"));
    const char *t = type && strncmp(type, "System:Device.", 14) == 0 ? type + 14 : type;
    out(c, "Device: %s  [type=%s%s%s%s%s%s]\n", name, t ? t : "?", group && *group ? ", group=" : "", group ? group : "",
        tdv_b(cJSON_GetObjectItemCaseSensitive(a, "IsGsd"), 0) ? ", GSD" : "", cm && *cm ? ", comment=" : "", cm ? cm : "");
    cJSON_Delete(a);
    td_clear_err();
}

static int a_get_device(tool_ctx *c)
{
    dev_ref d = { 0 };
    if (find_device(c, &d) != 0)
        return -1;
    device_header(c, d.device, d.name, d.group);
    print_ctx p = { c, 2 };
    tree_walk w = { (int)arg_i(c, "depth", 3), 0, emit_line, &p, 0 };
    walk_level(&w, d.device, 0, NULL);
    if (w.lines >= MAX_LINES)
        out(c, "(truncated at %d items)\n", MAX_LINES);
    return 0;
}

/* ---- network ------------------------------------------------------------------------------ */

typedef struct unconnected_ctx {
    tool_ctx *c;
    int count;
} unconnected_ctx;

static int unconnected_item(void *ctx, th h, const cJSON *it, int depth)
{
    (void)depth;
    unconnected_ctx *u = ctx;
    th ni = td_service(h, T_NETIF);
    if (!ni) {
        td_clear_err();
        return 0;
    }
    cJSON *nodes = td_enum(td_get_h(ni, "Nodes"), "Name,NodeType,Address,ConnectedSubnet", -1);
    const cJSON *x;
    cJSON_ArrayForEach(x, nodes)
    {
        if (tdv_h(tdi_a(x, "ConnectedSubnet")))
            continue;
        char owner[600];
        owner_text(tdv_h(x), owner, sizeof owner);
        const char *addr = tdi_s(x, "Address");
        out(u->c, "  %s (%s)  [type=%s, address=%s]\n", owner, tdi_s(x, "Name") ? tdi_s(x, "Name") : "?",
            tdi_s(x, "NodeType") ? tdi_s(x, "NodeType") : "?", addr && *addr ? addr : "(not set)");
        u->count++;
    }
    cJSON_Delete(nodes);
    td_clear_err();
    (void)it;
    return 0;
}

static int unconnected_device(void *ctx, th device, const cJSON *item, const char *group)
{
    (void)item;
    (void)group;
    nav_each_device_item(device, 3, unconnected_item, ctx);
    return 0;
}

static void print_network(tool_ctx *c)
{
    cJSON *subnets = td_enum(td_get_h(session_project(), "Subnets"), "Name,NetType", -1);
    int nsub = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, subnets)
    {
        nsub++;
        th sh = tdv_h(s);
        out(c, "Subnet %s  [type=%s]\n", tdi_s(s, "Name") ? tdi_s(s, "Name") : "?", tdi_s(s, "NetType") ? tdi_s(s, "NetType") : "?");
        cJSON *nodes = td_enum(td_get_h(sh, "Nodes"), "Name,Address,SubnetMask,PnDeviceName", -1);
        const cJSON *x;
        cJSON_ArrayForEach(x, nodes)
        {
            char owner[600];
            owner_text(tdv_h(x), owner, sizeof owner);
            const char *addr = tdi_s(x, "Address");
            const char *pn = tdi_s(x, "PnDeviceName");
            out(c, "  %s (%s)  [address=%s%s%s]\n", owner, tdi_s(x, "Name") ? tdi_s(x, "Name") : "?", addr && *addr ? addr : "(not set)",
                pn && *pn ? ", pnName=" : "", pn && *pn ? pn : "");
        }
        cJSON_Delete(nodes);
        cJSON *ios = td_enum(td_get_h(sh, "IoSystems"), "Name,Number", -1);
        cJSON_ArrayForEach(x, ios)
        {
            char ctl[600];
            owner_text(tdv_h(x), ctl, sizeof ctl); /* IO system -> IO controller -> interface -> ... */
            cJSON *devs = td_enum(td_get_h(tdv_h(x), "ConnectedIoDevices"), NULL, -1);
            strbuf names;
            sb_init(&names);
            const cJSON *d;
            int nd = 0;
            cJSON_ArrayForEach(d, devs)
            {
                char o[600];
                owner_text(tdv_h(d), o, sizeof o);
                sb_printf(&names, "%s%s", nd++ ? "; " : "", o);
            }
            cJSON_Delete(devs);
            out(c, "  IO system %s (%lld)  [controller=%s, devices=%d%s%s]\n", tdi_s(x, "Name") ? tdi_s(x, "Name") : "?",
                tdi_i(x, "Number", 0), ctl, nd, nd ? ": " : "", sb_str(&names));
            sb_free(&names);
        }
        cJSON_Delete(ios);
    }
    cJSON_Delete(subnets);
    if (!nsub)
        out(c, "(no subnets)\n");
    unconnected_ctx u = { c, 0 };
    out(c, "Interfaces not connected to a subnet:\n");
    nav_each_device(session_project(), unconnected_device, &u);
    if (!u.count)
        out(c, "  (none)\n");
    td_clear_err();
}

static int a_get_network(tool_ctx *c)
{
    print_network(c);
    return 0;
}

/* ---- full configuration --------------------------------------------------------------------- */

typedef struct full_ctx {
    tool_ctx *c;
    int devices;
    int lines;
} full_ctx;

static int full_device(void *ctx, th device, const cJSON *item, const char *group)
{
    full_ctx *f = ctx;
    if (f->lines >= MAX_LINES)
        return 1;
    device_header(f->c, device, tdi_s(item, "Name") ? tdi_s(item, "Name") : "?", group);
    print_ctx p = { f->c, 2 };
    tree_walk w = { 1, 1, emit_line, &p, f->lines };
    walk_level(&w, device, 0, NULL);
    f->lines = w.lines + 1;
    f->devices++;
    return 0;
}

static int a_get_full_config(tool_ctx *c)
{
    full_ctx f = { c, 0, 0 };
    nav_each_device(session_project(), full_device, &f);
    if (f.lines >= MAX_LINES)
        out(c, "(truncated: use get_device per device)\n");
    out(c, "%d device(s).\n\n", f.devices);
    print_network(c);
    return 0;
}

/* ---- I/O addresses ---------------------------------------------------------------------------- */

typedef struct io_row {
    char device[256];
    char module[256];
    int slot;
    char item[256];
    char type_name[128];
    char order[64];
    char channels[96];
    char plc[256];
    hw_addr a;
} io_row;

typedef struct plc_tags {
    char name[256];
    th sw;
    tag_spans sp;
} plc_tags;

typedef struct io_map {
    io_row *v;
    int n, cap;
    plc_tags plcs[32];
    int nplcs;
} io_map;

static const tag_spans *tags_of(io_map *m, const char *plc)
{
    for (int i = 0; i < m->nplcs; i++)
        if (strcmp(m->plcs[i].name, plc) == 0)
            return m->plcs[i].sw ? &m->plcs[i].sp : NULL;
    return NULL;
}

/* PLC (software name) controlling an address, registered in the map. */
static void address_plc(io_map *m, th address, char *out, size_t cap)
{
    out[0] = 0;
    cJSON *ctl = td_enum(td_get_h(address, "AddressControllers"), "Name", 1);
    th ac = cJSON_GetArraySize(ctl) ? tdv_h(cJSON_GetArrayItem(ctl, 0)) : 0;
    cJSON_Delete(ctl);
    th cpu = ac ? td_get_h(ac, "OwnedBy") : 0; /* AddressController feature -> CPU DeviceItem */
    th sw = cpu ? nav_plc_software(cpu) : 0;
    char *name = sw ? td_get_s(sw, "Name") : NULL;
    td_clear_err();
    if (!name)
        return;
    snprintf(out, cap, "%s", name);
    free(name);
    for (int i = 0; i < m->nplcs; i++)
        if (strcmp(m->plcs[i].name, out) == 0)
            return;
    if (m->nplcs < 32) {
        plc_tags *p = &m->plcs[m->nplcs++];
        memset(p, 0, sizeof *p);
        snprintf(p->name, sizeof p->name, "%s", out);
        p->sw = sw;
        ta_load(sw, &p->sp, 0);
    }
}

static void io_level(io_map *m, const char *device, th parent, int depth, const hw_item *module)
{
    cJSON *items = td_enum(td_get_h(parent, "DeviceItems"), ITEM_ATTRS, -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        hw_item *o = malloc(sizeof *o);
        if (!o)
            break;
        read_item(tdv_h(it), it, o);
        const hw_item *mod = depth == 0 ? o : module;
        if (o->naddr) {
            char ch[96];
            fmt_channels(o, ch, sizeof ch);
            cJSON *addrs = td_enum(td_get_h(o->h, "Addresses"), "IoType", -1);
            int idx = 0;
            const cJSON *x;
            cJSON_ArrayForEach(x, addrs)
            {
                const char *io = tdi_s(x, "IoType");
                if (!io || (strcmp(io, "Input") != 0 && strcmp(io, "Output") != 0) || idx >= o->naddr)
                    continue;
                if (m->n == m->cap) {
                    int cap = m->cap ? m->cap * 2 : 64;
                    io_row *p = realloc(m->v, (size_t)cap * sizeof *p);
                    if (!p)
                        break;
                    m->v = p;
                    m->cap = cap;
                }
                io_row *r = &m->v[m->n++];
                memset(r, 0, sizeof *r);
                snprintf(r->device, sizeof r->device, "%s", device);
                snprintf(r->module, sizeof r->module, "%s", mod->name);
                r->slot = mod->pos;
                snprintf(r->item, sizeof r->item, "%s", o->name);
                snprintf(r->type_name, sizeof r->type_name, "%s", o->type_name[0] ? o->type_name : mod->type_name);
                snprintf(r->order, sizeof r->order, "%s", o->order[0] ? o->order : mod->order);
                snprintf(r->channels, sizeof r->channels, "%s", ch);
                r->a = o->addr[idx++];
                address_plc(m, tdv_h(x), r->plc, sizeof r->plc);
            }
            cJSON_Delete(addrs);
        }
        if (depth < 4)
            io_level(m, device, o->h, depth + 1, mod);
        free(o);
    }
    cJSON_Delete(items);
    td_clear_err();
}

typedef struct io_dev_ctx {
    io_map *m;
    const char *only; /* device name filter (resolved) */
} io_dev_ctx;

static int io_device(void *ctx, th device, const cJSON *item, const char *group)
{
    (void)group;
    io_dev_ctx *k = ctx;
    const char *name = tdi_s(item, "Name");
    if (k->only && (!name || strcmp(name, k->only) != 0))
        return 0;
    io_level(k->m, name ? name : "?", device, 0, NULL);
    return 0;
}

static int row_cmp(const void *a, const void *b)
{
    const io_row *x = a, *y = b;
    int c = strcmp(x->plc, y->plc);
    if (c)
        return c;
    if (x->a.io != y->a.io)
        return x->a.io - y->a.io;
    return x->a.start < y->a.start ? -1 : x->a.start > y->a.start;
}

static void load_io(io_map *m, const char *only_device)
{
    memset(m, 0, sizeof *m);
    io_dev_ctx k = { m, only_device };
    nav_each_device(session_project(), io_device, &k);
    if (m->n > 1)
        qsort(m->v, (size_t)m->n, sizeof *m->v, row_cmp);
}

static void free_io(io_map *m)
{
    for (int i = 0; i < m->nplcs; i++)
        ta_free(&m->plcs[i].sp);
    free(m->v);
    memset(m, 0, sizeof *m);
}

static void span_text(const tag_span *s, char *out, size_t cap)
{
    if (s->bit >= 0)
        snprintf(out, cap, "%%%c%ld.%d", s->area, s->byte, s->bit);
    else
        snprintf(out, cap, "%%%c%c%ld", s->area, s->size == 1 ? 'B' : s->size == 2 ? 'W' : s->size == 4 ? 'D' : 'L', s->byte);
}

/* Tags of the controlling PLC inside the address range of a row. */
typedef int (*row_tag_fn)(void *ctx, const tag_span *s);
static int row_tags(io_map *m, const io_row *r, row_tag_fn fn, void *ctx)
{
    const tag_spans *sp = r->plc[0] ? tags_of(m, r->plc) : NULL;
    if (!sp)
        return 0;
    int bytes = r->a.bits > 0 ? (r->a.bits + 7) / 8 : 1;
    tag_span probe = { r->a.io, r->a.start, -1, bytes, "", "", "" };
    int n = 0;
    for (int i = 0; i < sp->n; i++)
        if (ta_overlaps(&sp->v[i], &probe)) {
            n++;
            if (fn && fn(ctx, &sp->v[i]))
                break;
        }
    return n;
}

typedef struct tag_list {
    strbuf *sb;
    int shown, max;
} tag_list;

static int list_tag(void *ctx, const tag_span *s)
{
    tag_list *t = ctx;
    if (t->shown < t->max) {
        char a[48];
        span_text(s, a, sizeof a);
        sb_printf(t->sb, "%s%s=%s", t->shown ? ", " : "", s->tag, a);
    }
    t->shown++;
    return 0;
}

static int a_get_io_map(tool_ctx *c)
{
    dev_ref d = { 0 };
    const char *dn = arg_s(c, "deviceName");
    if (dn && *dn && find_device(c, &d) != 0)
        return -1;
    io_map m;
    load_io(&m, dn && *dn ? d.name : NULL);
    long in_bytes = 0, out_bytes = 0;
    char last_plc[256] = "\x01";
    for (int i = 0; i < m.n; i++) {
        const io_row *r = &m.v[i];
        if (strcmp(last_plc, r->plc) != 0) {
            out(c, "%s:\n", r->plc[0] ? r->plc : "(no controlling PLC)");
            snprintf(last_plc, sizeof last_plc, "%s", r->plc);
        }
        char a[64];
        fmt_addr(&r->a, a, sizeof a);
        strbuf tags;
        sb_init(&tags);
        tag_list tl = { &tags, 0, 12 };
        int nt = row_tags(&m, r, list_tag, &tl);
        out(c, "  %s  %s / %s%s%s  [slot=%d%s%s%s%s, tags=%d%s%s%s]\n", a, r->device, r->module,
            strcmp(r->item, r->module) ? " / " : "", strcmp(r->item, r->module) ? r->item : "", r->slot,
            r->type_name[0] ? ", type=" : "", r->type_name, r->channels[0] ? ", channels=" : "", r->channels, nt,
            nt ? ": " : "", sb_str(&tags), nt > tl.max ? ", ..." : "");
        sb_free(&tags);
        long bytes = r->a.bits > 0 ? (r->a.bits + 7) / 8 : 0;
        if (r->a.io == 'I')
            in_bytes += bytes;
        else
            out_bytes += bytes;
    }
    out(c, "%d address range(s): inputs %ld byte(s), outputs %ld byte(s).%s\n", m.n, in_bytes, out_bytes,
        m.n ? " Tags = PLC tags whose address lies in the range." : "");
    free_io(&m);
    return 0;
}

/* ---- rack / slots --------------------------------------------------------------------------- */

typedef struct slot_entry {
    hw_item it;
    int index;
} slot_entry;

static int slot_cmp(const void *a, const void *b)
{
    const slot_entry *x = a, *y = b;
    return x->it.pos != y->it.pos ? x->it.pos - y->it.pos : x->index - y->index;
}

static int a_get_rack_slot_details(tool_ctx *c)
{
    dev_ref d = { 0 };
    if (find_device(c, &d) != 0)
        return -1;
    device_header(c, d.device, d.name, d.group);
    cJSON *items = td_enum(td_get_h(d.device, "DeviceItems"), ITEM_ATTRS, -1);
    int n = cJSON_GetArraySize(items);
    slot_entry *v = calloc((size_t)(n ? n : 1), sizeof *v);
    if (!v) {
        cJSON_Delete(items);
        return fail(c, "out of memory");
    }
    int k = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        v[k].index = k;
        read_item(tdv_h(it), it, &v[k++].it);
    }
    cJSON_Delete(items);
    qsort(v, (size_t)k, sizeof *v, slot_cmp);
    io_map m;
    load_io(&m, d.name);
    for (int i = 0; i < k; i++) {
        const hw_item *o = &v[i].it;
        strbuf sb;
        sb_init(&sb);
        if (o->cls[0])
            sb_printf(&sb, "class=%s, ", o->cls);
        sb_printf(&sb, "%s", o->type_name[0] ? o->type_name : "-");
        if (o->order[0])
            sb_printf(&sb, ", order=%s", o->order);
        if (o->fw[0])
            sb_printf(&sb, ", fw=%s", o->fw);
        int ntags = 0;
        for (int j = 0; j < m.n; j++) {
            const io_row *r = &m.v[j];
            if (strcmp(r->module, o->name) != 0 || r->slot != o->pos)
                continue;
            char a[64];
            fmt_addr(&r->a, a, sizeof a);
            sb_printf(&sb, ", %s%s%s%s", a, strcmp(r->item, r->module) ? " (" : "", strcmp(r->item, r->module) ? r->item : "",
                      strcmp(r->item, r->module) ? ")" : "");
            if (r->channels[0])
                sb_printf(&sb, " (%s)", r->channels);
            ntags += row_tags(&m, r, NULL, NULL);
        }
        if (ntags)
            sb_printf(&sb, ", tags=%d", ntags);
        out(c, "  slot %d  %s  [%s]\n", o->pos, o->name, sb_str(&sb));
        sb_free(&sb);
    }
    if (!k)
        out(c, "  (no modules)\n");
    out(c, "%d slot(s). I/Q ranges are byte addresses; get_io_map lists the tags per range.\n", k);
    free_io(&m);
    free(v);
    return 0;
}

/* ---- compile ----------------------------------------------------------------------------------- */

static int a_compile_device(tool_ctx *c)
{
    dev_ref d = { 0 };
    if (find_device(c, &d) != 0)
        return -1;
    out(c, "Compiling %s (hardware and software of the device):\n", d.name);
    return compile_object(c, d.device, arg_b(c, "errorsOnly", 0));
}

typedef struct compile_all_ctx {
    tool_ctx *c;
    int compiled, skipped, failed;
} compile_all_ctx;

static int compile_one(void *ctx, th device, const cJSON *item, const char *group)
{
    (void)group;
    compile_all_ctx *k = ctx;
    if (cancelled(k->c))
        return 1;
    th comp = td_service(device, "Siemens.Engineering.Compiler.ICompilable");
    td_clear_err();
    const char *name = tdi_s(item, "Name") ? tdi_s(item, "Name") : "?";
    if (!comp) {
        k->skipped++;
        return 0;
    }
    out(k->c, "== %s\n", name);
    progress(k->c, k->compiled, 0, name);
    if (compile_object(k->c, device, arg_b(k->c, "errorsOnly", 1)) != 0) {
        /* keep going with the other devices */
        k->c->is_error = 0;
        k->failed++;
    }
    k->compiled++;
    return 0;
}

static int a_compile_all(tool_ctx *c)
{
    compile_all_ctx k = { c, 0, 0, 0 };
    nav_each_device(session_project(), compile_one, &k);
    out(c, "%d device(s) compiled%s", k.compiled, k.failed ? "" : ".");
    if (k.failed)
        out(c, ", %d could not be compiled (see above).", k.failed);
    if (k.skipped)
        out(c, " %d device(s) without a compiler service skipped.", k.skipped);
    out(c, "\n");
    return 0;
}

/* ---- CSV exports -------------------------------------------------------------------------------- */

static void project_name(char *out, size_t cap)
{
    char *n = td_get_s(session_project(), "Name");
    snprintf(out, cap, "%s", n ? n : "project");
    free(n);
    for (char *p = out; *p; p++)
        if (strchr("\\/:*?\"<>| ", *p))
            *p = '_';
}

typedef struct csv_ctx {
    strbuf *sb;
    const char *device;
    const char *group;
    int rows;
} csv_ctx;

static void emit_csv(tree_walk *w, int depth, const hw_item *o, const char *net, const char *plc)
{
    csv_ctx *k = w->ctx;
    char in[160] = "", outq[160] = "", ch[96];
    for (int i = 0; i < o->naddr; i++) {
        char a[64];
        fmt_addr(&o->addr[i], a, sizeof a);
        char *dst = o->addr[i].io == 'I' ? in : outq;
        size_t len = strlen(dst);
        snprintf(dst + len, 160 - len, "%s%s", len ? " " : "", a + 2);
    }
    fmt_channels(o, ch, sizeof ch);
    char slot[16];
    snprintf(slot, sizeof slot, "%d", o->pos);
    char lvl[8];
    snprintf(lvl, sizeof lvl, "%d", depth);
    const char *cols[] = { k->device, k->group, o->name, lvl, slot, o->cls, o->type_name, o->order, o->fw, in, outq, ch,
                           plc, net, o->comment };
    for (size_t i = 0; i < sizeof cols / sizeof cols[0]; i++) {
        if (i)
            sb_appendc(k->sb, ';');
        csv_field(k->sb, cols[i]);
    }
    sb_append(k->sb, "\r\n");
    k->rows++;
}

static int csv_device(void *ctx, th device, const cJSON *item, const char *group)
{
    csv_ctx *k = ctx;
    k->device = tdi_s(item, "Name") ? tdi_s(item, "Name") : "?";
    k->group = group;
    tree_walk w = { 3, 1, emit_csv, k, 0 };
    walk_level(&w, device, 0, NULL);
    return 0;
}

static int export_devices(tool_ctx *c, int xlsx)
{
    strbuf sb;
    sb_init(&sb);
    sb_append(&sb, "Device;Group;Item;Level;Slot;Class;TypeName;OrderNumber;Firmware;Inputs;Outputs;Channels;PLC;Network;Comment");
    sb_append(&sb, "\r\n");
    csv_ctx k = { &sb, NULL, NULL, 0 };
    nav_each_device(session_project(), csv_device, &k);
    char pn[200], name[300];
    project_name(pn, sizeof pn);
    snprintf(name, sizeof name, "%s_devices", pn);
    out(c, "%d row(s).\n", k.rows);
    int rc = deliver_table(c, &sb, name, "Devices", xlsx);
    sb_free(&sb);
    return rc;
}

static int a_export_csv(tool_ctx *c)
{
    return export_devices(c, 0);
}

static int a_export_xlsx(tool_ctx *c)
{
    return export_devices(c, 1);
}

typedef struct csv_tag_ctx {
    strbuf *sb;
    const io_row *r;
    const char *const *base;
    int nbase;
    int rows;
} csv_tag_ctx;

static void io_base_cols(const io_row *r, char cols[][64], const char **base)
{
    snprintf(cols[0], 64, "%d", r->slot);
    snprintf(cols[1], 64, "%c", r->a.io);
    snprintf(cols[2], 64, "%ld", r->a.start);
    snprintf(cols[3], 64, "%ld", r->a.start + (r->a.bits > 0 ? (r->a.bits - 1) / 8 : 0));
    snprintf(cols[4], 64, "%d", r->a.bits);
    base[0] = r->device;
    base[1] = r->module;
    base[2] = cols[0];
    base[3] = r->item;
    base[4] = r->type_name;
    base[5] = r->order;
    base[6] = cols[1];
    base[7] = cols[2];
    base[8] = cols[3];
    base[9] = cols[4];
    base[10] = r->channels;
    base[11] = r->plc;
}

static void put_row(strbuf *sb, const char *const *base, int nbase, const tag_span *s)
{
    for (int i = 0; i < nbase; i++) {
        if (i)
            sb_appendc(sb, ';');
        csv_field(sb, base[i]);
    }
    char a[48] = "";
    if (s)
        span_text(s, a, sizeof a);
    sb_appendc(sb, ';');
    csv_field(sb, s ? s->tag : "");
    sb_appendc(sb, ';');
    csv_field(sb, a);
    sb_appendc(sb, ';');
    csv_field(sb, s ? s->type : "");
    sb_appendc(sb, ';');
    csv_field(sb, s ? s->table : "");
    sb_append(sb, "\r\n");
}

static int csv_tag(void *ctx, const tag_span *s)
{
    csv_tag_ctx *k = ctx;
    put_row(k->sb, k->base, k->nbase, s);
    k->rows++;
    return 0;
}

static int a_export_io_map(tool_ctx *c)
{
    int xlsx;
    if (table_format(c, &xlsx) != 0)
        return -1;
    dev_ref d = { 0 };
    const char *dn = arg_s(c, "deviceName");
    if (dn && *dn && find_device(c, &d) != 0)
        return -1;
    io_map m;
    load_io(&m, dn && *dn ? d.name : NULL);
    strbuf sb;
    sb_init(&sb);
    sb_append(&sb, "\xEF\xBB\xBF" "Device;Module;Slot;Item;TypeName;OrderNumber;Area;StartByte;EndByte;Bits;Channels;PLC;"
                   "Tag;TagAddress;TagType;TagTable\r\n");
    int rows = 0;
    for (int i = 0; i < m.n; i++) {
        char cols[5][64];
        const char *base[12];
        io_base_cols(&m.v[i], cols, base);
        csv_tag_ctx k = { &sb, &m.v[i], base, 12, 0 };
        if (row_tags(&m, &m.v[i], csv_tag, &k) == 0) {
            put_row(&sb, base, 12, NULL);
            k.rows++;
        }
        rows += k.rows;
    }
    char pn[200], name[400];
    project_name(pn, sizeof pn);
    snprintf(name, sizeof name, "%s_%s_io_map", pn, dn && *dn ? d.name : "all");
    out(c, "%d address range(s), %d row(s).\n", m.n, rows);
    free_io(&m);
    int rc = deliver_table(c, &sb, name, "IO map", xlsx);
    sb_free(&sb);
    return rc;
}

/* ---- CAx (AutomationML) hardware map ---------------------------------------------------------- */

typedef struct aml_ctx {
    strbuf *sb;
    int rows;
} aml_ctx;

static const char *aml_value(mxml_node_t *attr)
{
    const char *v = sml_child_text(attr, "Value");
    return v ? v : "";
}

static void aml_element(aml_ctx *k, mxml_node_t *ie, int level, const char *parent_path)
{
    const char *name = sml_attr(ie, "Name");
    char path[2048];
    snprintf(path, sizeof path, "%s%s%s", parent_path, *parent_path ? " / " : "", name ? name : "?");
    char type_name[256] = "", type_id[256] = "", fw[64] = "", pos[16] = "", kind[64] = "", label[64] = "";
    strbuf addrs, net, other;
    sb_init(&addrs);
    sb_init(&net);
    sb_init(&other);
    for (mxml_node_t *a = sml_child(ie, "Attribute"); a; a = sml_next(a, "Attribute")) {
        const char *an = sml_attr(a, "Name");
        if (!an)
            continue;
        const char *v = aml_value(a);
        if (strcmp(an, "TypeName") == 0)
            snprintf(type_name, sizeof type_name, "%s", v);
        else if (strcmp(an, "TypeIdentifier") == 0)
            snprintf(type_id, sizeof type_id, "%s", strncmp(v, "OrderNumber:", 12) == 0 ? v + 12 : v);
        else if (strcmp(an, "FirmwareVersion") == 0)
            snprintf(fw, sizeof fw, "%s", v);
        else if (strcmp(an, "PositionNumber") == 0)
            snprintf(pos, sizeof pos, "%s", v);
        else if (strcmp(an, "DeviceItemType") == 0)
            snprintf(kind, sizeof kind, "%s", v);
        else if (strcmp(an, "Label") == 0)
            snprintf(label, sizeof label, "%s", v);
        else if (strcmp(an, "Address") == 0) {
            for (mxml_node_t *e = sml_child(a, "Attribute"); e; e = sml_next(e, "Attribute")) {
                hw_addr ad = { 0, 0, 0 };
                for (mxml_node_t *f = sml_child(e, "Attribute"); f; f = sml_next(f, "Attribute")) {
                    const char *fn = sml_attr(f, "Name");
                    const char *fv = aml_value(f);
                    if (fn && strcmp(fn, "StartAddress") == 0)
                        ad.start = atol(fv);
                    else if (fn && strcmp(fn, "Length") == 0)
                        ad.bits = atoi(fv);
                    else if (fn && strcmp(fn, "IoType") == 0)
                        ad.io = strcmp(fv, "Input") == 0 ? 'I' : strcmp(fv, "Output") == 0 ? 'Q' : '?';
                }
                char t[64];
                fmt_addr(&ad, t, sizeof t);
                sb_printf(&addrs, "%s%s", addrs.len ? " " : "", t);
            }
        } else if (strcmp(an, "NetworkAddress") == 0 || strcmp(an, "SubnetMask") == 0 || strcmp(an, "ProfinetDeviceName") == 0 ||
                   strcmp(an, "IpProtocolSelection") == 0 || strcmp(an, "RouterAddress") == 0) {
            if (*v)
                sb_printf(&net, "%s%s=%s", net.len ? ", " : "", an, v);
        } else if (*v && strcmp(an, "BuiltIn") != 0 && strcmp(an, "InstallationDate") != 0) {
            sb_printf(&other, "%s%s=%s", other.len ? ", " : "", an, v);
        }
    }
    hw_item ch;
    memset(&ch, 0, sizeof ch);
    for (mxml_node_t *x = sml_child(ie, "ExternalInterface"); x; x = sml_next(x, "ExternalInterface")) {
        const char *ref = sml_attr(x, "RefBaseClassPath");
        if (!ref || !strstr(ref, "/Channel"))
            continue;
        const char *type = "", *io = "";
        for (mxml_node_t *a = sml_child(x, "Attribute"); a; a = sml_next(a, "Attribute")) {
            const char *an = sml_attr(a, "Name");
            if (an && strcmp(an, "Type") == 0)
                type = aml_value(a);
            else if (an && strcmp(an, "IoType") == 0)
                io = aml_value(a);
        }
        int dig = strcmp(type, "Digital") == 0, ana = strcmp(type, "Analog") == 0, in = strcmp(io, "Input") == 0;
        if (dig)
            in ? ch.di++ : ch.dq++;
        else if (ana)
            in ? ch.ai++ : ch.aq++;
        else if (ch.other_ch++ == 0)
            snprintf(ch.other_type, sizeof ch.other_type, "%s", *type ? type : "other");
    }
    char chs[96];
    fmt_channels(&ch, chs, sizeof chs);
    char lvl[8];
    snprintf(lvl, sizeof lvl, "%d", level);
    const char *cols[] = { lvl, path, name ? name : "", type_name, type_id, fw, pos, kind, label, sb_str(&addrs), chs,
                           sb_str(&net), sb_str(&other) };
    for (size_t i = 0; i < sizeof cols / sizeof cols[0]; i++) {
        if (i)
            sb_appendc(k->sb, ';');
        csv_field(k->sb, cols[i]);
    }
    sb_append(k->sb, "\r\n");
    k->rows++;
    sb_free(&addrs);
    sb_free(&net);
    sb_free(&other);
    for (mxml_node_t *sub = sml_child(ie, "InternalElement"); sub; sub = sml_next(sub, "InternalElement"))
        aml_element(k, sub, level + 1, path);
}

static int a_export_hardware_map(tool_ctx *c)
{
    int xlsx;
    if (table_format(c, &xlsx) != 0)
        return -1;
    dev_ref d = { 0 };
    const char *dn = arg_s(c, "deviceName");
    if (dn && *dn && find_device(c, &d) != 0)
        return -1;
    th cax = td_service(session_project(), "Siemens.Engineering.Cax.CaxProvider");
    if (!cax)
        return fail_td(c, "the CAx export service is not available");
    char dir[TC_PATH_MAX], aml[TC_PATH_MAX], log[TC_PATH_MAX];
    if (fs_temp_dir("cax", dir, sizeof dir) != 0)
        return fail(c, "cannot create a temporary folder");
    fs_join(aml, sizeof aml, dir, "hardware.aml");
    fs_join(log, sizeof log, dir, "export.log");
    progress(c, 0, 0, "CAx export");
    cJSON *ok = dn && *dn ? td_call_sig(cax, "Export", "Device,FileInfo,FileInfo", tda("hff", d.device, aml, log))
                          : td_call_sig(cax, "Export", "Project,FileInfo,FileInfo", tda("hff", session_project(), aml, log));
    int exported = ok && cJSON_IsTrue(ok);
    cJSON_Delete(ok);
    char last[512] = "";
    char *logtext = NULL;
    size_t loglen = 0;
    if (fs_read_all(log, &logtext, &loglen) == 0) {
        /* last non-empty line: "Exporting of the CAx data is completed (errors: 0, warnings: 0)" */
        char *end = logtext + loglen;
        while (end > logtext && (end[-1] == '\n' || end[-1] == '\r'))
            end--;
        char *start = end;
        while (start > logtext && start[-1] != '\n')
            start--;
        snprintf(last, sizeof last, "%.*s", (int)(end - start), start);
        free(logtext);
    }
    if (!exported || !fs_is_file(aml)) {
        int rc = td_failed() ? fail_td(c, "CAx export failed") : fail(c, "CAx export failed%s%s", *last ? ": " : "", last);
        fs_remove_tree(dir);
        return rc;
    }
    char err[256];
    mxml_node_t *top = sml_load_file(aml, err, sizeof err);
    if (!top) {
        fs_remove_tree(dir);
        return fail(c, "cannot read the AML file: %s", err);
    }
    strbuf sb;
    sb_init(&sb);
    sb_append(&sb, "\xEF\xBB\xBF" "Level;Path;Name;TypeName;TypeIdentifier;Firmware;Position;Kind;Label;Addresses;Channels;"
                   "Network;OtherAttributes\r\n");
    aml_ctx k = { &sb, 0 };
    mxml_node_t *root = sml_find(top, "CAEXFile");
    for (mxml_node_t *ih = root ? sml_child(root, "InstanceHierarchy") : NULL; ih; ih = sml_next(ih, "InstanceHierarchy"))
        for (mxml_node_t *ie = sml_child(ih, "InternalElement"); ie; ie = sml_next(ie, "InternalElement"))
            aml_element(&k, ie, 0, "");
    mxmlDelete(top);
    fs_remove_tree(dir);
    char pn[200], name[400];
    project_name(pn, sizeof pn);
    snprintf(name, sizeof name, "%s_%s_hardware_map", pn, dn && *dn ? d.name : "all");
    /* "05.10.2026 02:21:24 PM: INFO : Exporting of the CAx data is completed (errors: 0, warnings: 0)" */
    const char *msg = strstr(last, " : ");
    out(c, "%d element(s) from the CAx (AutomationML) export.%s%s\n", k.rows, *last ? " TIA log: " : "", msg ? msg + 3 : last);
    int rc = deliver_table(c, &sb, name, "Hardware map", xlsx);
    sb_free(&sb);
    return rc;
}

/* ---- network settings ------------------------------------------------------------------------- */

static int valid_ipv4(const char *s)
{
    int parts = 0;
    while (*s) {
        if (!isdigit((unsigned char)*s))
            return 0;
        long v = strtol(s, (char **)&s, 10);
        if (v < 0 || v > 255)
            return 0;
        parts++;
        if (*s == '.')
            s++;
        else if (*s)
            return 0;
    }
    return parts == 4;
}

typedef struct node_pick {
    const char *wanted;
    th node;
    char iface[256];
    char node_name[64];
    strbuf available;
} node_pick;

static int pick_node(void *ctx, th h, const cJSON *it, int depth)
{
    (void)depth;
    node_pick *p = ctx;
    th ni = td_service(h, T_NETIF);
    if (!ni) {
        td_clear_err();
        return 0;
    }
    cJSON *nodes = td_enum(td_get_h(ni, "Nodes"), "Name,NodeType", -1);
    const cJSON *x;
    int stop = 0;
    cJSON_ArrayForEach(x, nodes)
    {
        const char *type = tdi_s(x, "NodeType");
        const char *nn = tdi_s(x, "Name");
        const char *in = tdi_s(it, "Name");
        if (!type || strcmp(type, "Ethernet") != 0)
            continue;
        sb_printf(&p->available, "%s%s (%s)", p->available.len ? ", " : "", in ? in : "?", nn ? nn : "?");
        if (p->wanted && *p->wanted && !(in && _stricmp(in, p->wanted) == 0) && !(nn && _stricmp(nn, p->wanted) == 0))
            continue;
        if (!p->node) {
            p->node = tdv_h(x);
            snprintf(p->iface, sizeof p->iface, "%s", in ? in : "?");
            snprintf(p->node_name, sizeof p->node_name, "%s", nn ? nn : "?");
            stop = 1;
        }
    }
    cJSON_Delete(nodes);
    td_clear_err();
    return stop && p->wanted && *p->wanted ? 1 : 0;
}

typedef struct net_change {
    const char *arg;  /* tool argument */
    const char *attr; /* Node attribute */
    char kind;        /* s string, b bool, e enum name */
} net_change;

static const net_change k_changes[] = {
    { "pnDeviceNameAutoGeneration", "PnDeviceNameAutoGeneration", 'b' },
    { "useRouter", "UseRouter", 'b' },
    { "ipProtocolSelection", "IpProtocolSelection", 'e' },
    { "ipAddress", "Address", 's' },
    { "subnetMask", "SubnetMask", 's' },
    { "routerAddress", "RouterAddress", 's' },
    { "useIsoProtocol", "UseIsoProtocol", 'b' },
    { "pnDeviceName", "PnDeviceName", 's' },
};

static int a_set_network_config(tool_ctx *c)
{
    dev_ref d = { 0 };
    if (find_device(c, &d) != 0)
        return -1;
    node_pick p = { arg_s(c, "interfaceName"), 0, "", "", { 0 } };
    sb_init(&p.available);
    nav_each_device_item(d.device, 3, pick_node, &p);
    if (!p.node) {
        int rc = p.available.len ? fail(c, "interface '%s' not found on %s. Ethernet interfaces: %s", p.wanted, d.name, sb_str(&p.available))
                                 : fail(c, "%s has no Ethernet interface", d.name);
        sb_free(&p.available);
        return rc;
    }
    sb_free(&p.available);
    for (size_t i = 0; i < sizeof k_changes / sizeof k_changes[0]; i++) {
        const char *v = arg_s(c, k_changes[i].arg);
        if (k_changes[i].kind == 's' && v && *v && strcmp(k_changes[i].arg, "pnDeviceName") != 0 && !valid_ipv4(v))
            return fail(c, "%s '%s' is not a valid IPv4 address", k_changes[i].arg, v);
    }
    /* planned values: explicit arguments plus implied ones */
    const char *planned[sizeof k_changes / sizeof k_changes[0]] = { 0 };
    char bools[sizeof k_changes / sizeof k_changes[0]][8];
    int nchanges = 0;
    for (size_t i = 0; i < sizeof k_changes / sizeof k_changes[0]; i++) {
        if (!arg_has(c, k_changes[i].arg))
            continue;
        if (k_changes[i].kind == 'b') {
            snprintf(bools[i], sizeof bools[i], "%s", arg_b(c, k_changes[i].arg, 0) ? "true" : "false");
            planned[i] = bools[i];
        } else {
            planned[i] = arg_s(c, k_changes[i].arg);
            if (!planned[i] || !*planned[i])
                return fail(c, "%s must not be empty", k_changes[i].arg);
        }
        nchanges++;
    }
    if (planned[7] && !planned[0]) /* pnDeviceName set directly: turn off the automatic name */
        planned[0] = "false";
    if (planned[5] && !planned[1]) /* routerAddress implies useRouter */
        planned[1] = "true";
    const char *station = arg_s(c, "stationName");
    if (station && *station)
        nchanges++;
    if (!nchanges)
        return fail(c, "nothing to change: pass ipAddress, subnetMask, useRouter, routerAddress, ipProtocolSelection, "
                       "useIsoProtocol, pnDeviceNameAutoGeneration, pnDeviceName or stationName");
    out(c, "%s / %s (%s):\n", d.name, p.iface, p.node_name);
    for (size_t i = 0; i < sizeof k_changes / sizeof k_changes[0]; i++) {
        if (!planned[i])
            continue;
        cJSON *cur = td_get(p.node, k_changes[i].attr);
        char *t = cur ? tdv_text(cur) : NULL;
        int same = t && _stricmp(t, planned[i]) == 0;
        if (same && !arg_has(c, k_changes[i].arg))
            planned[i] = NULL; /* implied value already in place */
        else if (same)
            out(c, "  %s: %s (unchanged)\n", k_changes[i].attr, t);
        else
            out(c, "  %s: %s -> %s\n", k_changes[i].attr, t && *t ? t : "(empty)", planned[i]);
        free(t);
        cJSON_Delete(cur);
        td_clear_err();
    }
    if (station && *station)
        out(c, "  station (device) name: %s -> %s\n", d.name, station);
    if (!confirmed(c, "yes") && !confirmed(c, "I understand"))
        return fail(c, "preview only: repeat with confirm='yes' to apply these changes to the project");
    for (size_t i = 0; i < sizeof k_changes / sizeof k_changes[0]; i++) {
        if (!planned[i])
            continue;
        cJSON *v = k_changes[i].kind == 'b' ? cJSON_CreateBool(strcmp(planned[i], "true") == 0) : cJSON_CreateString(planned[i]);
        if (td_set(p.node, k_changes[i].attr, v) != 0) {
            char what[128];
            snprintf(what, sizeof what, "setting %s failed", k_changes[i].attr);
            return fail_td(c, what);
        }
    }
    if (station && *station && td_set(d.device, "Name", cJSON_CreateString(station)) != 0)
        return fail_td(c, "renaming the device failed");
    if (station && *station)
        nav_cache_clear();
    out(c, "Applied to the project. Download the hardware configuration to bring the change to the device.\n");
    return 0;
}

/* ---- hardware catalog ----------------------------------------------------------------------------- */

#define CATALOG_ATTRS "ArticleNumber,Version,TypeName,TypeIdentifier,CatalogPath,Description"

static int a_search_catalog(tool_ctx *c)
{
    const char *filter = arg_req(c, "filter");
    if (!filter)
        return -1;
    int limit = (int)arg_i(c, "limit", 50);
    if (limit < 1)
        limit = 1;
    th cat = td_get_h(session_portal(), "HardwareCatalog");
    progress(c, 0, 0, "searching the hardware catalog");
    th list = cat ? td_call_h(cat, "Find", tda("s", filter)) : 0;
    if (!list)
        return fail_td(c, "catalog search failed");
    long long total = 0;
    td_get_i(list, "Count", &total);
    cJSON *items = td_enum(list, CATALOG_ATTRS, limit);
    strbuf sb;
    sb_init(&sb);
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    dc_format_entry(&sb, tdi_s(it, "ArticleNumber"), tdi_s(it, "Version"), tdi_s(it, "TypeName"), tdi_s(it, "TypeIdentifier"),
                    tdi_s(it, "CatalogPath"));
    cJSON_Delete(items);
    out_raw(c, sb_str(&sb));
    sb_free(&sb);
    out(c, "%lld match(es)%s. typeIdentifier is what a device or module is created with.\n", total,
        total > limit ? " (refine the filter or raise limit)" : "");
    return 0;
}

int hw_dump_catalog(tool_ctx *c)
{
    th cat = td_get_h(session_portal(), "HardwareCatalog");
    progress(c, 0, 0, "reading the whole hardware catalog (about a minute)");
    th list = cat ? td_call_h(cat, "Find", tda("s", "")) : 0;
    if (!list)
        return fail_td(c, "reading the catalog failed");
    cJSON *items = td_enum(list, CATALOG_ATTRS, -1);
    if (!items)
        return fail_td(c, "reading the catalog entries failed");
    strbuf sb;
    sb_init(&sb);
    char ver[32], when[32];
    tia_portal_version(ver, sizeof ver);
    time_t now = time(NULL);
    struct tm tm;
    localtime_s(&tm, &now);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
    sb_printf(&sb, "# TIA Portal %s hardware catalog, %d entries, %s\n", *ver ? ver : "?", cJSON_GetArraySize(items), when);
    static const char *const cols[] = { "ArticleNumber", "Version", "TypeName", "TypeIdentifier", "CatalogPath", "Description" };
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        for (int i = 0; i < 6; i++) {
            const char *v = tdi_s(it, cols[i]);
            char buf[1024];
            snprintf(buf, sizeof buf, "%s", v ? v : "");
            dc_clean_field(buf);
            sb_printf(&sb, "%s%s", i ? "\t" : "", buf);
        }
        sb_appendc(&sb, '\n');
    }
    int n = cJSON_GetArraySize(items);
    cJSON_Delete(items);
    int rc = dc_catalog_write(sb.p, sb.len);
    sb_free(&sb);
    if (rc != 0)
        return fail(c, "cannot write the local catalog");
    char path[TC_PATH_MAX];
    dc_catalog_path(path, sizeof path);
    out(c, "%d catalog entries written to %s. Search them without TIA Portal with admin action=search_device_catalog.\n", n, path);
    return 0;
}

/* ---- tool ------------------------------------------------------------------------------------------- */

static const action_def actions[] = {
    { "compile_all", "optional errorsOnly=true",
      "Compile every device (hardware and software) and report errors per device. 'Not compilable' means nothing "
      "needed compiling.",
      a_compile_all, AF_PROJECT },
    { "compile_device", "deviceName; optional errorsOnly=false",
      "Compile one device (hardware and software) and list the compiler messages.", a_compile_device, AF_PROJECT },
    { "dump_catalog", "",
      "Copy the whole TIA Portal hardware catalog (~12000 entries, about a minute) to a local file, searchable without TIA "
      "Portal with admin action=search_device_catalog. Run it again after installing an HSP or a TIA update.",
      hw_dump_catalog, AF_PORTAL },
    { "export_csv", "optional outputPath, returnInline",
      "Device list as CSV (one row per module / interface: slot, type, order number, firmware, I/Q bytes, channels, network). "
      "After export, ask the user whether to open the file (admin action=open_file).",
      a_export_csv, AF_PROJECT },
    { "export_hardware_map", "optional format=csv|xlsx, deviceName, outputPath, returnInline",
      "Raw CAx (AutomationML) hardware data as a flat table: every element of the station (rack, CPU, modules, "
      "submodules, interfaces, ports) with type, order number, firmware, position, addresses, channels and network "
      "attributes. Without deviceName the whole project. After export, ask the user whether to open the file.",
      a_export_hardware_map, AF_PROJECT },
    { "export_io_map", "optional format=csv|xlsx, deviceName, outputPath, returnInline",
      "Complete I/O map as CSV or Excel: one row per address range and PLC tag inside it (device, module, slot, area, start/end "
      "byte, channels, controlling PLC, tag, tag address, type, table). Without deviceName all devices. After export, "
      "ask the user whether to open the file.",
      a_export_io_map, AF_PROJECT },
    { "export_xlsx", "optional outputPath",
      "Device list as an Excel workbook (same columns as export_csv). After export, ask the user whether to open the file.",
      a_export_xlsx, AF_PROJECT },
    { "get_device", "deviceName; optional depth=3",
      "Detailed configuration of one device (station): every item with slot, type, order number, firmware, I/Q addresses, "
      "channels, network interfaces (address, subnet, PROFINET name, IO system role) and comments. deviceName may also "
      "be the name of the CPU or interface module.",
      a_get_device, AF_PROJECT },
    { "get_full_config", "",
      "Configuration of all devices (modules, interfaces and addresses; built-in items without content are skipped) "
      "followed by the network overview of get_network.",
      a_get_full_config, AF_PROJECT },
    { "get_io_map", "optional deviceName",
      "I/O address map across all devices (or one), grouped by controlling PLC: address range, device / module, slot, "
      "type, channels and the PLC tags whose address lies in the range.",
      a_get_io_map, AF_PROJECT },
    { "get_network", "",
      "Subnets with their nodes (address, PROFINET name), IO systems with controller and IO devices, and the interfaces "
      "not connected to any subnet.",
      a_get_network, AF_PROJECT },
    { "get_rack_slot_details", "deviceName",
      "Rack/slot topology of one device: per slot the module, order number, firmware, I/Q byte ranges, channels and the "
      "number of PLC tags assigned to its addresses.",
      a_get_rack_slot_details, AF_PROJECT },
    { "search_catalog", "filter; optional limit=50",
      "Live search in the TIA Portal hardware catalog (article number or type name, e.g. '6ES7 521-1BH' or 'CPU 1511'): "
      "type name, article number, version, typeIdentifier and catalog path. The first search in a TIA Portal session "
      "loads the catalog (about a minute); admin action=search_device_catalog searches the local copy instantly.",
      a_search_catalog, AF_PORTAL },
    { "set_network_config",
      "deviceName, confirm; optional interfaceName, ipAddress, subnetMask, useRouter, routerAddress, ipProtocolSelection, "
      "useIsoProtocol, pnDeviceNameAutoGeneration, pnDeviceName, stationName",
      "Set the network configuration of a device's Ethernet interface (first one, or interfaceName = interface item or "
      "node name such as X1). pnDeviceName sets the PROFINET device name directly (turns off the automatic name). "
      "stationName renames the device. Without confirm='yes' the planned changes are only shown. Project only: "
      "download the hardware configuration afterwards.",
      a_set_network_config, AF_PROJECT | AF_WRITES },
};

const tool_def tool_hardware = {
    .name = "hardware",
    .title = "Hardware configuration",
    .summary = "Hardware configuration: devices, rack/slot topology, networks and IO systems, I/O address map with tags, "
               "hardware compile, CSV and CAx exports, network settings and the hardware catalog.",
    .properties =
        "{"
        "\"deviceName\":{\"type\":\"string\",\"description\":\"Device (station) name from session list_devices, or the "
        "name of its CPU / interface module.\"},"
        "\"depth\":{\"type\":\"integer\"},\"errorsOnly\":{\"type\":\"boolean\"},\"limit\":{\"type\":\"integer\"},"
        "\"format\":{\"type\":\"string\",\"enum\":[\"csv\",\"xlsx\"]},\"outputPath\":{\"type\":\"string\"},"
        "\"returnInline\":{\"type\":\"boolean\"},"
        "\"filter\":{\"type\":\"string\",\"description\":\"search_catalog: article number or type name fragment.\"},"
        "\"confirm\":{\"type\":\"string\"},\"interfaceName\":{\"type\":\"string\"},"
        "\"ipAddress\":{\"type\":\"string\"},\"subnetMask\":{\"type\":\"string\"},\"useRouter\":{\"type\":\"boolean\"},"
        "\"routerAddress\":{\"type\":\"string\"},"
        "\"ipProtocolSelection\":{\"type\":\"string\",\"enum\":[\"Project\",\"Dhcp\",\"UserProgram\",\"OtherPath\","
        "\"ViaIoController\",\"None\"],\"description\":\"Project = IP set in the project, OtherPath = set directly at the "
        "device.\"},"
        "\"useIsoProtocol\":{\"type\":\"boolean\"},\"pnDeviceNameAutoGeneration\":{\"type\":\"boolean\"},"
        "\"pnDeviceName\":{\"type\":\"string\"},\"stationName\":{\"type\":\"string\"}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
};
