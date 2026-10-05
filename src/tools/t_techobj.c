/* technology_objects: technology objects of a PLC (motion axes, encoders, cams,
   PID controllers, counters, ...): list, create, delete, rename, parameters,
   hardware connections, compile, SimaticML export/import. */
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

#define T_AXIS_HW "Siemens.Engineering.SW.TechnologicalObjects.Motion.AxisHardwareConnectionProvider"
#define T_ENCODER_HW "Siemens.Engineering.SW.TechnologicalObjects.Motion.EncoderHardwareConnectionProvider"
#define T_MI_HW "Siemens.Engineering.SW.TechnologicalObjects.Motion.MeasuringInputHardwareConnectionProvider"
#define T_CAM_HW "Siemens.Engineering.SW.TechnologicalObjects.Motion.OutputCamHardwareConnectionProvider"
#define T_SUBOBJECTS "Siemens.Engineering.SW.TechnologicalObjects.Motion.OutputCamMeasuringInputContainer"
#define TO_ATTRS "Name,Number,OfSystemLibElement,OfSystemLibVersion"

/* ---- capabilities (static, needs no TIA) ---------------------------------------------------------- */

static const struct {
    const char *family;
    const char *cpus;
    const char *versions; /* accepted by Create in TIA Portal V21 (S7-1500 CPU), "" = not verified */
    const char *what;
} k_families[] = {
    { "TO_SpeedAxis", "S7-1500, S7-1500T, ET 200SP CPU", "7.0 8.0 9.0 10.0", "speed-controlled axis" },
    { "TO_PositioningAxis", "S7-1200, S7-1500, S7-1500T, ET 200SP CPU", "7.0 8.0 9.0 10.0", "positioning axis" },
    { "TO_SynchronousAxis", "S7-1500, S7-1500T, ET 200SP CPU", "7.0 8.0 9.0 10.0", "synchronous axis (gearing; camming on T-CPUs)" },
    { "TO_ExternalEncoder", "S7-1500, S7-1500T, ET 200SP CPU", "7.0 8.0 9.0 10.0", "external encoder" },
    { "TO_MeasuringInput", "S7-1500, S7-1500T", "", "measuring input (belongs to an axis or encoder)" },
    { "TO_OutputCam", "S7-1500, S7-1500T", "", "output cam (belongs to an axis or encoder)" },
    { "TO_CamTrack", "S7-1500, S7-1500T", "", "cam track (belongs to an axis or encoder)" },
    { "TO_Cam", "S7-1500T", "", "cam disk" },
    { "TO_Cam_10k", "S7-1500T", "", "cam disk with up to 10000 points" },
    { "TO_Kinematics", "S7-1500T", "", "kinematics" },
    { "TO_LeadingAxisProxy", "S7-1500T", "", "leading axis proxy (cross-PLC synchronous operation)" },
    { "TO_Interpreter", "S7-1500T", "", "motion interpreter" },
    { "TO_CommandTable", "S7-1200", "", "command table" },
    { "PID_Compact", "S7-1200, S7-1500", "2.4 3.0", "universal PID controller" },
    { "PID_3Step", "S7-1200, S7-1500", "2.3", "PID controller for valves/actuators with integral behaviour" },
    { "PID_Temp", "S7-1200, S7-1500", "1.1 2.0", "temperature PID controller (heating/cooling)" },
    { "High_Speed_Counter", "S7-1500 with TM Count / TM PosInput, ET 200SP", "3.0 4.0 5.0", "counting and measuring" },
    { "SSI_Absolute_Encoder", "S7-1500 with TM PosInput", "3.0", "SSI absolute encoder" },
};

static int a_capabilities(tool_ctx *c)
{
    const char *f = arg_s(c, "family");
    int n = 0;
    for (size_t i = 0; i < sizeof k_families / sizeof k_families[0]; i++) {
        if (f && *f) {
            char pattern[128];
            snprintf(pattern, sizeof pattern, glob_has_wildcards(f) ? "%s" : "*%s*", f);
            if (!glob_match(pattern, k_families[i].family, 0))
                continue;
        }
        out(c, "%s  [cpus=%s, versions=%s] %s\n", k_families[i].family, k_families[i].cpus,
            *k_families[i].versions ? k_families[i].versions : "not verified", k_families[i].what);
        n++;
    }
    if (!n)
        out(c, "(no family matches '%s')\n", f);
    out(c, "family is the type identifier passed to create, version the technology version. The versions listed were "
           "accepted by TIA Portal V21 on an S7-1500 CPU; TIA does not list the valid ones when a version is refused. "
           "Compiling aligns the version to the CPU firmware (compile reports before -> after). The T-CPU families and "
           "TO_CommandTable need a CPU that supports them.\n");
    return 0;
}

/* ---- lookup ------------------------------------------------------------------------------------------ */

typedef struct to_ref {
    th item;
    char name[256];
    char family[96];
    char version[32];
    char parent[256]; /* owner axis/encoder for sub-objects */
} to_ref;

static void read_ref(const cJSON *it, to_ref *o)
{
    memset(o, 0, sizeof *o);
    o->item = tdv_h(it);
    snprintf(o->name, sizeof o->name, "%s", tdi_s(it, "Name") ? tdi_s(it, "Name") : "?");
    snprintf(o->family, sizeof o->family, "%s", tdi_s(it, "OfSystemLibElement") ? tdi_s(it, "OfSystemLibElement") : "?");
    char *v = tdv_text(tdi_a(it, "OfSystemLibVersion"));
    snprintf(o->version, sizeof o->version, "%s", v ? v : "?");
    free(v);
}

typedef int (*to_fn)(void *ctx, const to_ref *o, const char *folder);

typedef struct to_walk {
    to_fn fn;
    void *ctx;
    int stop;
} to_walk;

static void walk_sub(to_walk *w, th owner, const char *owner_name, const char *folder)
{
    th cont = td_service(owner, T_SUBOBJECTS);
    if (!cont) {
        td_clear_err();
        return;
    }
    static const char *const comps[] = { "OutputCams", "MeasuringInputs" };
    for (int i = 0; i < 2 && !w->stop; i++) {
        cJSON *subs = td_enum(td_get_h(cont, comps[i]), TO_ATTRS, -1);
        const cJSON *it;
        cJSON_ArrayForEach(it, subs)
        {
            to_ref o;
            read_ref(it, &o);
            snprintf(o.parent, sizeof o.parent, "%s", owner_name);
            if ((w->stop = w->fn(w->ctx, &o, folder)) != 0)
                break;
        }
        cJSON_Delete(subs);
    }
    td_clear_err();
}

static int walk_item(void *ctx, const cJSON *item, const char *folder)
{
    to_walk *w = ctx;
    to_ref o;
    read_ref(item, &o);
    if ((w->stop = w->fn(w->ctx, &o, folder)) != 0)
        return 1;
    walk_sub(w, o.item, o.name, folder);
    return w->stop;
}

static void walk_tos(th plc_software, to_fn fn, void *ctx)
{
    to_walk w = { fn, ctx, 0 };
    sw_walk(plc_software, SWC_TECH_OBJECTS, TO_ATTRS, walk_item, NULL, &w);
    td_clear_err();
}

typedef struct find_to {
    const char *wanted;
    to_ref *out;
    int found;
    strbuf names;
} find_to;

static int find_fn(void *ctx, const to_ref *o, const char *folder)
{
    (void)folder;
    find_to *f = ctx;
    if (f->names.len < 2000)
        sb_printf(&f->names, "%s%s", f->names.len ? ", " : "", o->name);
    if (_stricmp(o->name, f->wanted) == 0) {
        *f->out = *o;
        f->found = 1;
        return 1;
    }
    return 0;
}

static int find_to_named(tool_ctx *c, const nav_plc *p, const char *name, to_ref *out)
{
    find_to f = { name, out, 0, { 0 } };
    sb_init(&f.names);
    walk_tos(p->software, find_fn, &f);
    if (!f.found)
        fail(c, "technology object '%s' not found in %s. Objects: %s", name, p->device_name, f.names.len ? sb_str(&f.names) : "(none)");
    else
        snprintf(c->error_context, sizeof c->error_context, "device=%s to=%s", p->device_name, out->name);
    sb_free(&f.names);
    return f.found ? 0 : -1;
}

static int resolve(tool_ctx *c, nav_plc *p, to_ref *o)
{
    if (sw_plc(c, p) != 0)
        return -1;
    const char *name = arg_req(c, "name");
    return name ? find_to_named(c, p, name, o) : -1;
}

/* ---- read actions ------------------------------------------------------------------------------------ */

typedef struct list_ctx {
    tool_ctx *c;
    int n, limit;
} list_ctx;

static int list_fn(void *ctx, const to_ref *o, const char *folder)
{
    list_ctx *k = ctx;
    if (k->n++ >= k->limit)
        return 1;
    long long num = 0;
    td_get_i(o->item, "Number", &num);
    td_clear_err();
    out(k->c, "%s%s%s  [type=%s, version=%s, num=%lld%s%s]\n", folder && *folder ? folder : "", folder && *folder ? "/" : "", o->name,
        o->family, o->version, num, o->parent[0] ? ", parent=" : "", o->parent);
    return 0;
}

static int a_list(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    list_ctx k = { c, 0, (int)arg_i(c, "limit", 500) };
    walk_tos(p.software, list_fn, &k);
    if (!k.n)
        out(c, "(no technology objects in %s)\n", p.device_name);
    else if (k.n > k.limit)
        out(c, "(truncated at %d - raise limit)\n", k.limit);
    else
        out(c, "%d technology object(s) in %s.\n", k.n, p.device_name);
    return 0;
}

static char *param_text(const cJSON *v)
{
    if (!v || cJSON_IsNull(v))
        return _strdup("(null)");
    if (tdv_is_err(v)) {
        const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(v, "$err"));
        char buf[300];
        snprintf(buf, sizeof buf, "(not readable: %s)", e ? e : "?");
        return _strdup(buf);
    }
    return tdv_text(v);
}

static const char *param_type(const cJSON *v)
{
    if (!v || cJSON_IsNull(v))
        return "null";
    if (cJSON_IsBool(v))
        return "Bool";
    if (cJSON_IsNumber(v)) /* the wire does not keep Int vs Real */
        return "number";
    if (cJSON_IsString(v))
        return "string";
    const char *t = tdv_type(v);
    const char *dot = t ? strrchr(t, '.') : NULL;
    return dot ? dot + 1 : (t ? t : "object");
}

static int a_get_parameters(tool_ctx *c)
{
    nav_plc p;
    to_ref o;
    if (resolve(c, &p, &o) != 0)
        return -1;
    const char *prefix = arg_s(c, "prefix");
    int summary = arg_b(c, "summary", 0);
    int limit = (int)arg_i(c, "limit", 300);
    progress(c, 0, 0, "reading parameters (the first read of an object can take several seconds)");
    cJSON *params = td_enum(td_get_h(o.item, "Parameters"), "Name,Value", -1);
    if (!params)
        return fail_td(c, "reading the parameters failed");
    out(c, "%s  [type=%s, version=%s] - %d parameter(s)%s%s\n", o.name, o.family, o.version, cJSON_GetArraySize(params),
        prefix && *prefix ? ", prefix " : "", prefix ? prefix : "");
    cJSON *areas = cJSON_CreateObject();
    int shown = 0, matched = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, params)
    {
        const char *n = tdi_s(it, "Name");
        if (!n || (prefix && *prefix && _strnicmp(n, prefix, strlen(prefix)) != 0))
            continue;
        matched++;
        if (summary) {
            char area[128];
            size_t len = strcspn(n + (prefix ? strlen(prefix) : 0), ".");
            snprintf(area, sizeof area, "%.*s", (int)(len + (prefix ? strlen(prefix) : 0)), n);
            cJSON *cnt = cJSON_GetObjectItemCaseSensitive(areas, area);
            if (cnt)
                cJSON_SetNumberValue(cnt, cnt->valuedouble + 1);
            else
                cJSON_AddNumberToObject(areas, area, 1);
            continue;
        }
        if (shown >= limit)
            continue;
        const cJSON *v = tdi_a(it, "Value");
        char *t = param_text(v);
        out(c, "  %s = %s  [%s]\n", n, t ? t : "?", param_type(v));
        free(t);
        shown++;
    }
    if (summary) {
        cJSON *a;
        cJSON_ArrayForEach(a, areas)
        out(c, "  %s  [%d]\n", a->string, (int)a->valuedouble);
        out(c, "%d parameter(s) in %d area(s). Use prefix to read one area.\n", matched, cJSON_GetArraySize(areas));
    } else if (matched > shown) {
        out(c, "... %d more (raise limit or narrow with prefix; summary=true lists the areas)\n", matched - shown);
    }
    cJSON_Delete(areas);
    cJSON_Delete(params);
    td_clear_err();
    out(c, "Openness does not report whether a parameter is writable: set_parameter refuses read-only ones with TIA's error.\n");
    return 0;
}

static th find_param(tool_ctx *c, const to_ref *o, const char *name)
{
    th params = td_get_h(o->item, "Parameters");
    th prm = params ? td_call_h(params, "Find", tda("s", name)) : 0;
    if (!prm) {
        td_clear_err();
        fail(c, "parameter '%s' not found in %s (names are case-sensitive, e.g. Actor.Type; see get_parameters)", name, o->name);
    }
    return prm;
}

static int a_find_parameter(tool_ctx *c)
{
    nav_plc p;
    to_ref o;
    if (resolve(c, &p, &o) != 0)
        return -1;
    const char *name = arg_req(c, "parameter");
    if (!name)
        return -1;
    th prm = find_param(c, &o, name);
    if (!prm)
        return -1;
    cJSON *v = td_get(prm, "Value");
    char *t = param_text(v);
    out(c, "%s.%s = %s  [%s]\n", o.name, name, t ? t : "?", param_type(v));
    free(t);
    cJSON_Delete(v);
    td_clear_err();
    return 0;
}

static int a_set_parameter(tool_ctx *c)
{
    nav_plc p;
    to_ref o;
    if (resolve(c, &p, &o) != 0)
        return -1;
    const char *name = arg_req(c, "parameter");
    const char *value = name ? arg_req(c, "value") : NULL;
    if (!value)
        return -1;
    if (!arg_has(c, "expectedCurrentValue"))
        return fail(c, "expectedCurrentValue is required: read the parameter with find_parameter first");
    const char *expected = arg_s(c, "expectedCurrentValue");
    th prm = find_param(c, &o, name);
    if (!prm)
        return -1;
    cJSON *cur = td_get(prm, "Value");
    char *before = param_text(cur);
    int same = before && expected && _stricmp(before, expected) == 0;
    if (!same && cJSON_IsNumber(cur) && expected) /* 1 vs 1.0 */
        same = strtod(expected, NULL) == cur->valuedouble;
    if (!same) {
        int rc = fail(c, "%s.%s is %s, not the expected %s: nothing changed (read it again with find_parameter)", o.name, name,
                      before ? before : "?", expected ? expected : "(empty)");
        free(before);
        cJSON_Delete(cur);
        return rc;
    }
    cJSON *nv;
    if (cJSON_IsBool(cur))
        nv = cJSON_CreateBool(_stricmp(value, "true") == 0 || strcmp(value, "1") == 0);
    else if (cJSON_IsNumber(cur))
        nv = cJSON_CreateNumber(strtod(value, NULL));
    else
        nv = cJSON_CreateString(value); /* strings and enum names */
    cJSON_Delete(cur);
    if (td_set(prm, "Value", nv) != 0) {
        free(before);
        return fail_td(c, "setting the parameter failed (read-only or invalid value)");
    }
    cJSON *after = td_get(prm, "Value");
    char *now = param_text(after);
    out(c, "%s.%s: %s -> %s\n", o.name, name, before ? before : "?", now ? now : "?");
    free(before);
    free(now);
    cJSON_Delete(after);
    td_clear_err();
    return 0;
}

/* ---- hardware connection ------------------------------------------------------------------------------ */

static void iface_line(tool_ctx *c, const char *label, th iface)
{
    cJSON *a = td_attrs(iface, "IsConnected,InputModule,OutputModule,InputOutputModule,InputAddress,OutputAddress,PathToDBMember,"
                               "Channel,ChannelIndex,OutputTag,SensorIndexInActorTelegram,ConnectOption");
    int connected = tdv_b(cJSON_GetObjectItemCaseSensitive(a, "IsConnected"), 0);
    strbuf sb;
    sb_init(&sb);
    static const char *const keys[] = { "InputOutputModule", "InputModule", "OutputModule", "InputAddress", "OutputAddress",
                                        "PathToDBMember", "Channel", "ChannelIndex", "OutputTag", "SensorIndexInActorTelegram",
                                        "ConnectOption" };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, keys[i]);
        if (!v || cJSON_IsNull(v) || tdv_is_err(v))
            continue;
        char *t = NULL;
        th h = tdv_h(v);
        if (h) {
            char *n = td_get_s(h, "Name");
            t = n ? n : _strdup("<object>");
        } else {
            t = tdv_text(v);
        }
        if (t && *t && strcmp(t, "0") != 0 && strcmp(t, "-1") != 0) /* -1 = no address */
            sb_printf(&sb, ", %s=%s", keys[i], t);
        free(t);
    }
    out(c, "  %s: %s%s\n", label, connected ? "connected" : "not connected", sb_str(&sb));
    sb_free(&sb);
    cJSON_Delete(a);
    td_clear_err();
}

static int a_get_hardware_connection(tool_ctx *c)
{
    nav_plc p;
    to_ref o;
    if (resolve(c, &p, &o) != 0)
        return -1;
    out(c, "%s  [type=%s, version=%s]\n", o.name, o.family, o.version);
    int any = 0;
    th axis = td_service(o.item, T_AXIS_HW);
    if (axis) {
        any = 1;
        iface_line(c, "actor", td_get_h(axis, "ActorInterface"));
        cJSON *sensors = td_enum(td_get_h(axis, "SensorInterface"), NULL, -1);
        int i = 1;
        const cJSON *s;
        cJSON_ArrayForEach(s, sensors)
        {
            char label[32];
            snprintf(label, sizeof label, "sensor %d", i++);
            iface_line(c, label, tdv_h(s));
        }
        cJSON_Delete(sensors);
        th torque = td_get_h(axis, "TorqueInterface");
        if (torque)
            iface_line(c, "torque", torque);
    }
    th enc = td_service(o.item, T_ENCODER_HW);
    if (enc) { /* a single interface for an external encoder */
        any = 1;
        iface_line(c, "sensor", td_get_h(enc, "SensorInterface"));
    }
    th mi = td_service(o.item, T_MI_HW);
    if (mi) {
        any = 1;
        iface_line(c, "measuring input", mi);
    }
    th cam = td_service(o.item, T_CAM_HW);
    if (cam) {
        any = 1;
        iface_line(c, "output cam", cam);
    }
    td_clear_err();
    if (!any)
        out(c, "  (no hardware connection applies to %s)\n", o.family);
    return 0;
}

typedef struct item_find {
    const char *wanted;
    th found;
    char device[256];
} item_find;

static int item_named(void *ctx, th h, const cJSON *it, int depth)
{
    (void)depth;
    item_find *f = ctx;
    const char *n = tdi_s(it, "Name");
    if (n && _stricmp(n, f->wanted) == 0) {
        f->found = h;
        return 1;
    }
    return 0;
}

static int device_items(void *ctx, th device, const cJSON *item, const char *group)
{
    (void)group;
    item_find *f = ctx;
    if (nav_each_device_item(device, 4, item_named, f)) {
        snprintf(f->device, sizeof f->device, "%s", tdi_s(item, "Name") ? tdi_s(item, "Name") : "?");
        return 1;
    }
    return 0;
}

static int a_connect_hardware(tool_ctx *c)
{
    nav_plc p;
    to_ref o;
    if (resolve(c, &p, &o) != 0)
        return -1;
    const char *iface = arg_req(c, "interface");
    const char *target = iface ? arg_s(c, "target") : NULL;
    if (!iface)
        return -1;
    th conn = 0;
    if (_stricmp(iface, "actor") == 0 || _stricmp(iface, "torque") == 0) {
        th axis = td_service(o.item, T_AXIS_HW);
        conn = axis ? td_get_h(axis, _stricmp(iface, "actor") == 0 ? "ActorInterface" : "TorqueInterface") : 0;
    } else if (_strnicmp(iface, "sensor", 6) == 0) {
        int idx = iface[6] ? atoi(iface + 6) : 1;
        th axis = td_service(o.item, T_AXIS_HW);
        if (axis) {
            cJSON *sensors = td_enum(td_get_h(axis, "SensorInterface"), NULL, -1);
            conn = idx >= 1 ? tdv_h(cJSON_GetArrayItem(sensors, idx - 1)) : 0;
            cJSON_Delete(sensors);
        } else {
            th enc = td_service(o.item, T_ENCODER_HW); /* external encoder: one sensor */
            conn = enc && idx == 1 ? td_get_h(enc, "SensorInterface") : 0;
        }
    } else if (_stricmp(iface, "measuring_input") == 0) {
        conn = td_service(o.item, T_MI_HW);
    } else if (_stricmp(iface, "output_cam") == 0) {
        conn = td_service(o.item, T_CAM_HW);
    } else {
        return fail(c, "interface must be actor, sensor1..4, torque, measuring_input or output_cam");
    }
    td_clear_err();
    if (!conn)
        return fail(c, "%s (%s) has no '%s' interface (see get_hardware_connection)", o.name, o.family, iface);
    if (!target || !*target || _stricmp(target, "none") == 0) {
        if (td_call_v(conn, "Disconnect", NULL) != 0)
            return fail_td(c, "disconnecting failed");
        out(c, "%s %s disconnected.\n", o.name, iface);
        return 0;
    }
    /* a device item (drive telegram, module, submodule) by name, otherwise a DB member path / tag name */
    item_find f = { target, 0, "" };
    nav_each_device(session_project(), device_items, &f);
    td_clear_err();
    int rc = f.found ? td_call_v(conn, "Connect", tda("h", f.found)) : td_call_v(conn, "Connect", tda("s", target));
    if (rc != 0)
        return fail_td(c, "connecting failed");
    if (f.found)
        out(c, "%s %s connected to device item '%s' of %s.\n", o.name, iface, target, f.device);
    else
        out(c, "%s %s connected to '%s' (DB member / tag by name).\n", o.name, iface, target);
    iface_line(c, iface, conn);
    return 0;
}

/* ---- create / delete / rename / compile ------------------------------------------------------------------ */

static th target_group(tool_ctx *c, const nav_plc *p)
{
    const char *path = arg_s(c, "path");
    th g = path && *path ? sw_folder(c, p->software, SWC_TECH_OBJECTS, path, 1, 1) : td_get_h(p->software, "TechnologicalObjectGroup");
    return g ? td_get_h(g, "TechnologicalObjects") : 0;
}

static int a_create(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    const char *name = arg_req(c, "name");
    const char *family = name ? arg_req(c, "family") : NULL;
    const char *version = family ? arg_req(c, "version") : NULL;
    if (!version)
        return -1;
    to_ref existing;
    find_to f = { name, &existing, 0, { 0 } };
    sb_init(&f.names);
    walk_tos(p.software, find_fn, &f);
    sb_free(&f.names);
    if (f.found)
        return fail(c, "technology object '%s' already exists in %s", name, p.device_name);
    th comp = target_group(c, &p);
    if (!comp)
        return c->is_error ? -1 : fail_td(c, "technology object folder not available");
    const char *ver = version[0] == 'V' || version[0] == 'v' ? version + 1 : version; /* the bridge converts it to System.Version */
    th to = td_call_h(comp, "Create", tda("sss", name, family, ver));
    if (!to)
        return fail_td(c, "creating the technology object failed (family and versions known to work: action=capabilities)");
    cJSON *a = td_attrs(to, TO_ATTRS);
    char *v = tdv_text(cJSON_GetObjectItemCaseSensitive(a, "OfSystemLibVersion"));
    out(c, "Technology object '%s' created in %s [type=%s, version=%s, num=%lld]. Family and version cannot be changed "
           "later.\n",
        name, p.device_name, tdv_s(cJSON_GetObjectItemCaseSensitive(a, "OfSystemLibElement")) ? tdv_s(cJSON_GetObjectItemCaseSensitive(a, "OfSystemLibElement")) : family,
        v ? v : version, tdv_i(cJSON_GetObjectItemCaseSensitive(a, "Number"), 0));
    free(v);
    cJSON_Delete(a);
    nav_cache_clear();
    return 0;
}

static int a_create_from_master_copy(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    const char *mcpath = arg_req(c, "masterCopy");
    if (!mcpath)
        return -1;
    /* master copy "Folder/Name" in the project library or in library=<open global library> */
    const char *libname = arg_s(c, "library");
    th lib = 0;
    if (!libname || !*libname || _stricmp(libname, "project") == 0) {
        lib = td_get_h(session_project(), "ProjectLibrary");
    } else {
        cJSON *libs = td_enum(td_get_h(session_portal(), "GlobalLibraries"), "Name", -1);
        const cJSON *it;
        cJSON_ArrayForEach(it, libs)
        if (tdi_s(it, "Name") && _stricmp(tdi_s(it, "Name"), libname) == 0)
            lib = tdv_h(it);
        cJSON_Delete(libs);
        if (!lib)
            return fail(c, "global library '%s' is not open (library action=open_global_library)", libname);
    }
    th folder = td_get_h(lib, "MasterCopyFolder");
    char buf[512];
    snprintf(buf, sizeof buf, "%s", mcpath);
    char *ctx = NULL;
    th mc = 0;
    for (char *seg = strtok_s(buf, "/", &ctx); seg && folder; seg = strtok_s(NULL, "/", &ctx)) {
        char *rest = ctx && *ctx ? ctx : NULL;
        if (!rest) {
            mc = td_call_h(td_get_h(folder, "MasterCopies"), "Find", tda("s", seg));
            break;
        }
        folder = td_call_h(td_get_h(folder, "Folders"), "Find", tda("s", seg));
    }
    td_clear_err();
    if (!mc)
        return fail(c, "master copy '%s' not found (library action=get_tree typeFilter=master_copies)", mcpath);
    th comp = target_group(c, &p);
    if (!comp)
        return c->is_error ? -1 : fail_td(c, "technology object folder not available");
    th to = td_call_h(comp, "CreateFrom", tda("h", mc));
    if (!to)
        return fail_td(c, "creating the technology object from the master copy failed");
    char *n = td_get_s(to, "Name");
    out(c, "Technology object '%s' created in %s from master copy '%s'.\n", n ? n : "?", p.device_name, mcpath);
    free(n);
    nav_cache_clear();
    return 0;
}

static int a_delete(tool_ctx *c)
{
    nav_plc p;
    to_ref o;
    if (resolve(c, &p, &o) != 0)
        return -1;
    if (!confirmed(c, "yes") && !confirmed(c, "I understand"))
        return fail(c, "deleting %s (%s) is not reversible outside this undo step: a re-created object starts from default "
                       "parameters. Repeat with confirm='yes'.",
                    o.name, o.family);
    if (td_call_v(o.item, "Delete", NULL) != 0)
        return fail_td(c, "deleting the technology object failed");
    nav_cache_clear();
    out(c, "Technology object '%s' (%s) deleted from %s.\n", o.name, o.family, p.device_name);
    return 0;
}

static int a_rename(tool_ctx *c)
{
    nav_plc p;
    to_ref o;
    if (resolve(c, &p, &o) != 0)
        return -1;
    const char *nn = arg_req(c, "newName");
    if (!nn)
        return -1;
    if (td_set(o.item, "Name", cJSON_CreateString(nn)) != 0)
        return fail_td(c, "renaming failed");
    nav_cache_clear();
    out(c, "Technology object '%s' renamed to '%s'. Program blocks reference technology objects by name and are NOT updated: "
           "check them with xref and compile.\n",
        o.name, nn);
    return 0;
}

typedef struct ver_ctx {
    tool_ctx *c;
    int phase; /* 0 before, 1 after */
    cJSON *before;
} ver_ctx;

static int version_fn(void *ctx, const to_ref *o, const char *folder)
{
    (void)folder;
    ver_ctx *k = ctx;
    if (k->phase == 0) {
        cJSON_AddStringToObject(k->before, o->name, o->version);
    } else {
        const char *b = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(k->before, o->name));
        out(k->c, "  %s  [type=%s, version %s%s%s]\n", o->name, o->family, b ? b : "?", b && strcmp(b, o->version) ? " -> " : "",
            b && strcmp(b, o->version) ? o->version : "");
    }
    return 0;
}

static int a_compile(tool_ctx *c)
{
    nav_plc p;
    to_ref o;
    if (resolve(c, &p, &o) != 0)
        return -1;
    const char *scope = arg_s(c, "scope");
    int group = scope && _stricmp(scope, "group") == 0;
    ver_ctx k = { c, 0, cJSON_CreateObject() };
    walk_tos(p.software, version_fn, &k);
    th target = o.item;
    if (group) /* the folder holding the object */
        target = td_get_h(o.item, "Parent");
    out(c, "Compiling %s:\n", group ? "the technology object folder" : o.name);
    int rc = compile_object(c, target, arg_b(c, "errorsOnly", 0));
    if (rc != 0 && group) { /* a folder may not be compilable: fall back to the object */
        c->is_error = 0;
        out(c, "(the folder cannot be compiled: compiling %s only)\n", o.name);
        rc = compile_object(c, o.item, arg_b(c, "errorsOnly", 0));
    }
    out(c, "Technology object versions:\n");
    k.phase = 1;
    walk_tos(p.software, version_fn, &k);
    cJSON_Delete(k.before);
    return rc;
}

static int a_show_in_editor(tool_ctx *c)
{
    nav_plc p;
    to_ref o;
    if (resolve(c, &p, &o) != 0)
        return -1;
    if (td_call_v(o.item, "ShowInEditor", NULL) != 0)
        return fail_td(c, "opening the editor failed (needs a TIA Portal with user interface)");
    out(c, "%s opened in the TIA Portal editor.\n", o.name);
    return 0;
}

/* ---- export / import ----------------------------------------------------------------------------------- */

static int a_export(tool_ctx *c)
{
    nav_plc p;
    to_ref o;
    if (resolve(c, &p, &o) != 0)
        return -1;
    char path[TC_PATH_MAX], name[400];
    if (sw_export_xml(c, o.item, path, sizeof path) != 0) {
        if (strstr(td_err(), "not supported") || strstr(td_err(), "version"))
            out(c, "SimaticML export of technology objects depends on the family and version (see TIA Portal help: "
                   "'Exporting and importing technology objects').\n");
        return -1;
    }
    snprintf(name, sizeof name, "%s.xml", o.name);
    const char *outp = arg_s(c, "outputPath");
    int rc = sw_deliver(c, path, name, "application/xml", outp, arg_b(c, "returnInline", outp && *outp ? 0 : 1));
    fs_remove(path);
    return rc;
}

static int a_import(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    const char *f = arg_s(c, "filePath");
    if (!f || !*f)
        f = arg_s(c, "file");
    const char *xml = arg_s(c, "xmlContent");
    char path[TC_PATH_MAX];
    int temp = 0;
    if (f && *f) {
        if (fs_full_path(f, path, sizeof path) != 0 || !fs_is_file(path))
            return fail(c, "file '%s' not found", f);
    } else if (xml && *xml) {
        if (fs_temp_path("to", ".xml", path, sizeof path) != 0 || fs_write_all(path, xml, strlen(xml)) != 0)
            return fail(c, "cannot write a temporary file");
        temp = 1;
    } else {
        return fail(c, "pass filePath or xmlContent (SimaticML of a technology object)");
    }
    th comp = target_group(c, &p);
    cJSON *res = comp ? td_call(comp, "Import", tda("fe", path, "Siemens.Engineering.ImportOptions", arg_b(c, "overwrite", 0) ? "Override" : "None")) : NULL;
    if (temp)
        fs_remove(path);
    if (!res)
        return c->is_error ? -1 : fail_td(c, "importing the technology object failed");
    strbuf names;
    sb_init(&names);
    th rh = tdv_h(res);
    cJSON *items = rh ? td_enum(rh, "Name", -1) : NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    sb_printf(&names, "%s%s", names.len ? ", " : "", tdi_s(it, "Name") ? tdi_s(it, "Name") : "?");
    cJSON_Delete(items);
    cJSON_Delete(res);
    td_clear_err();
    nav_cache_clear();
    out(c, "Imported into %s: %s.\n", p.device_name, names.len ? sb_str(&names) : "(done)");
    sb_free(&names);
    return 0;
}

/* ---- tool ------------------------------------------------------------------------------------------------- */

#define R AF_PROJECT
#define W (AF_PROJECT | AF_WRITES)

static const action_def actions[] = {
    { "capabilities", "optional family",
      "Technology object families (type identifiers for create), the CPUs that support them and what they are. Needs no "
      "TIA Portal connection.",
      a_capabilities, 0 },
    { "compile", "deviceName, name; optional scope=group, errorsOnly",
      "Compile a technology object and report every object version before and after (compiling aligns versions to the "
      "CPU firmware). scope=group compiles the folder, which TIA does by compiling the PLC software.",
      a_compile, R },
    { "connect_hardware", "deviceName, name, interface, target",
      "Connect an interface (actor, sensor1..4, torque, measuring_input, output_cam) to a device item (drive telegram, "
      "module or submodule, by name) or to a DB member / tag by name. target='none' disconnects. Sensor indices are 1-based.",
      a_connect_hardware, W },
    { "create", "deviceName, name, family, version; optional path",
      "Create a technology object, e.g. family=TO_PositioningAxis version=9.0 or family=PID_Compact. family and version "
      "are fixed at creation. path = technology object folder (created if missing).",
      a_create, W },
    { "create_from_master_copy", "deviceName, masterCopy; optional library='project', path",
      "Create a technology object from a master copy ('Folder/Name' in the project library or an open global library). "
      "Master copies of technology objects are made in the TIA Portal UI.",
      a_create_from_master_copy, W },
    { "delete", "deviceName, name, confirm",
      "Delete a technology object (confirm='yes'). A re-created object starts from default parameters.", a_delete,
      W | AF_DESTRUCTIVE },
    { "export", "deviceName, name; optional outputPath, returnInline",
      "Export a technology object as SimaticML (supported depending on family and version).", a_export, R },
    { "find_parameter", "deviceName, name, parameter", "One parameter: value and type.", a_find_parameter, R },
    { "get_hardware_connection", "deviceName, name",
      "Read-only wiring report: actor, sensors, torque, measuring input or output cam connections (module, addresses, DB "
      "member). A provider that does not apply to the family is reported as such.",
      a_get_hardware_connection, R },
    { "get_parameters", "deviceName, name; optional prefix, summary=false, limit=300",
      "Parameters of a technology object with value and type. prefix filters one area (e.g. Actor.); summary=true lists the "
      "areas with counts only. The first read of an object can take several seconds.",
      a_get_parameters, R },
    { "import", "deviceName, filePath | xmlContent; optional path, overwrite=false",
      "Import a technology object from SimaticML (as made by export).", a_import, W },
    { "list", "deviceName; optional limit=500",
      "All technology objects: Folder/Name [type=<family>, version, num, parent=<owner>]. Output cams, cam tracks and "
      "measuring inputs hang off their axis or encoder and are included.",
      a_list, R },
    { "rename", "deviceName, name, newName",
      "Rename a technology object. Program blocks reference it BY NAME and are NOT updated.", a_rename, W },
    { "set_parameter", "deviceName, name, parameter, value, expectedCurrentValue",
      "Write one parameter. expectedCurrentValue is required and must match the present value (read it with "
      "find_parameter), otherwise nothing is written. Echoes old -> new.",
      a_set_parameter, W },
    { "show_in_editor", "deviceName, name", "Open the technology object in the TIA Portal editor (TIA with user interface).",
      a_show_in_editor, R },
};

const tool_def tool_technology_objects = {
    .name = "technology_objects",
    .title = "Technology objects",
    .summary = "Technology objects of a PLC (motion axes and encoders, cams, PID controllers, counters): list, create, "
               "delete, rename, parameters, hardware connections, compile, SimaticML export/import.",
    .properties =
        "{"
        "\"deviceName\":{\"type\":\"string\",\"description\":\"Device or PLC name (session list_devices).\"},"
        "\"name\":{\"type\":\"string\",\"description\":\"Technology object name.\"},"
        "\"newName\":{\"type\":\"string\"},"
        "\"family\":{\"type\":\"string\",\"description\":\"Type identifier, e.g. TO_PositioningAxis, PID_Compact (see capabilities).\"},"
        "\"version\":{\"type\":\"string\",\"description\":\"Technology version, e.g. 9.0.\"},"
        "\"path\":{\"type\":\"string\",\"description\":\"Technology object folder.\"},"
        "\"masterCopy\":{\"type\":\"string\"},\"library\":{\"type\":\"string\"},"
        "\"parameter\":{\"type\":\"string\",\"description\":\"Parameter name, e.g. Actor.Type.\"},"
        "\"value\":{\"type\":[\"string\",\"number\",\"boolean\"]},\"expectedCurrentValue\":{\"type\":[\"string\",\"number\",\"boolean\"]},"
        "\"prefix\":{\"type\":\"string\"},\"summary\":{\"type\":\"boolean\"},\"limit\":{\"type\":\"integer\"},"
        "\"interface\":{\"type\":\"string\",\"description\":\"actor, sensor1..4, torque, measuring_input, output_cam.\"},"
        "\"target\":{\"type\":\"string\",\"description\":\"Device item name, DB member path or tag; 'none' disconnects.\"},"
        "\"scope\":{\"type\":\"string\",\"enum\":[\"object\",\"group\"]},\"errorsOnly\":{\"type\":\"boolean\"},"
        "\"confirm\":{\"type\":\"string\"},\"outputPath\":{\"type\":\"string\"},\"returnInline\":{\"type\":\"boolean\"},"
        "\"filePath\":{\"type\":\"string\"},\"file\":{\"type\":\"string\"},\"xmlContent\":{\"type\":\"string\"},"
        "\"overwrite\":{\"type\":\"boolean\"}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
};
