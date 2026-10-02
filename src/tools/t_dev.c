/* dev: development aids, registered only when TIACMD_DEV=1. */
#include "tools.h"

#include "tia/session.h"
#include "tia/tia_dyn.h"
#include "tia/tia_nav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int a_add_device(tool_ctx *c)
{
    const char *order = arg_req(c, "orderNumber");
    const char *name = order ? arg_req(c, "name") : NULL;
    if (!name)
        return -1;
    const char *dev = arg_s(c, "deviceName");
    th devices = td_get_h(session_project(), "Devices");
    char type_id[256];
    snprintf(type_id, sizeof type_id, "%s%s", strncmp(order, "OrderNumber:", 12) == 0 ? "" : "OrderNumber:", order);
    th d = td_call_h(devices, "CreateWithItem", tda("sss", type_id, name, dev && *dev ? dev : name));
    if (!d)
        return fail_td(c, "CreateWithItem failed");
    out(c, "Device created: %s (%s)\n", dev && *dev ? dev : name, type_id);
    return 0;
}

typedef struct var {
    char name[64];
    th h;
} var;

static th resolve_var(const char *n, var *vars, int nvars, tool_ctx *c)
{
    if (strcmp(n, "project") == 0)
        return session_project();
    if (strcmp(n, "portal") == 0)
        return session_portal();
    if (strncmp(n, "plc:", 4) == 0 || strncmp(n, "cpu:", 4) == 0) {
        nav_plc plc;
        if (nav_find_plc(c, session_project(), n + 4, &plc) != 0)
            return 0;
        return n[0] == 'p' ? plc.software : plc.cpu;
    }
    for (int i = 0; i < nvars; i++)
        if (strcmp(vars[i].name, n) == 0)
            return vars[i].h;
    return 0;
}

/* Replaces "$name" strings by handles: {"$h": id}, or a bare id for the "h" field. */
static void substitute(cJSON *v, var *vars, int nvars, tool_ctx *c)
{
    cJSON *child = v ? v->child : NULL;
    while (child) {
        cJSON *next = child->next;
        if (cJSON_IsString(child) && child->valuestring[0] == '$' && child->valuestring[1] != '$') {
            th h = resolve_var(child->valuestring + 1, vars, nvars, c);
            if (h) {
                cJSON *rep;
                if (child->string && strcmp(child->string, "h") == 0) {
                    rep = cJSON_CreateNumber((double)h);
                } else {
                    rep = cJSON_CreateObject();
                    cJSON_AddNumberToObject(rep, "$h", (double)h);
                }
                if (child->string)
                    cJSON_ReplaceItemInObjectCaseSensitive(v, child->string, rep);
                else
                    cJSON_ReplaceItemViaPointer(v, child, rep);
            }
        } else if (cJSON_IsObject(child) || cJSON_IsArray(child)) {
            substitute(child, vars, nvars, c);
        }
        child = next;
    }
}

static int a_eval(tool_ctx *c)
{
    const cJSON *ops = arg_arr(c, "ops");
    if (!ops)
        return fail(c, "ops must be an array of bridge requests");
    var vars[64];
    int nvars = 0;
    int i = 0;
    const cJSON *op;
    cJSON_ArrayForEach(op, ops)
    {
        cJSON *req = cJSON_Duplicate(op, 1);
        char as[64] = "";
        const char *a = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(req, "as"));
        if (a)
            snprintf(as, sizeof as, "%s", a);
        cJSON_DeleteItemFromObjectCaseSensitive(req, "as");
        substitute(req, vars, nvars, c);
        cJSON *res = td_request(req);
        char *s = res ? cJSON_PrintUnformatted(res) : NULL;
        out(c, "[%d] %s\n", i++, s ? s : td_err());
        cJSON_free(s);
        th h = res ? tdv_h(res) : 0;
        if (h && *as && nvars < 64) {
            snprintf(vars[nvars].name, sizeof vars[nvars].name, "%s", as);
            vars[nvars++].h = h;
        }
        cJSON_Delete(res);
    }
    const char *f = arg_s(c, "failWith"); /* exercises the transaction rollback */
    return f ? fail(c, "%s", f) : 0;
}

static const action_def actions[] = {
    { "add_device", "orderNumber, name; optional deviceName",
      "Create a device from the hardware catalog (e.g. 6ES7 511-1AL03-0AB0/V4.1) in the open project.", a_add_device,
      AF_PROJECT | AF_WRITES },
    { "eval", "ops; optional failWith",
      "Run raw bridge requests in sequence. Strings \"$project\", \"$portal\", \"$plc:<device>\" and \"$<as>\" become handles. "
      "failWith fails the call afterwards (the transaction rolls the changes back).",
      a_eval, AF_PROJECT | AF_WRITES },
};

const tool_def tool_dev = {
    .name = "dev",
    .title = "Development aids",
    .summary = "Development aids (only with TIACMD_DEV=1).",
    .properties = "{"
                  "\"orderNumber\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"deviceName\":{\"type\":\"string\"},"
                  "\"ops\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}},\"failWith\":{\"type\":\"string\"}"
                  "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
};
