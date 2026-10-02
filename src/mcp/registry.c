#include "registry.h"

#include "app/stats.h"
#include "tia/session.h"
#include "tia/tia_dyn.h"
#include "tools/tools.h"
#include "util/log.h"
#include "util/strbuf.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const tool_def *const g_tools[] = {
    &tool_get_info,
    &tool_session,
    &tool_admin,
    &tool_blocks_read,
    &tool_blocks_write,
    &tool_xref,
    &tool_folders,
    &tool_db,
    &tool_udt,
    &tool_tag,
    &tool_watch,
    &tool_diagnostics,
    &tool_download_upload,
    &tool_dev, /* must stay last: only exposed with TIACMD_DEV=1 */
};

static int tool_count(void)
{
    static int n = -1;
    if (n < 0) {
        const char *dev = getenv("TIACMD_DEV");
        int total = (int)(sizeof g_tools / sizeof g_tools[0]);
        n = dev && *dev && strcmp(dev, "0") != 0 ? total : total - 1;
    }
    return n;
}

#define NTOOLS tool_count()

int registry_count(void) { return NTOOLS; }
const tool_def *registry_at(int i) { return i >= 0 && i < NTOOLS ? g_tools[i] : NULL; }

const tool_def *registry_find(const char *name)
{
    for (int i = 0; i < NTOOLS; i++)
        if (strcmp(g_tools[i]->name, name) == 0)
            return g_tools[i];
    return NULL;
}

int registry_action_total(void)
{
    int n = 0;
    for (int i = 0; i < NTOOLS; i++)
        n += g_tools[i]->nactions;
    return n;
}

static char *describe(const tool_def *t)
{
    strbuf sb;
    sb_init(&sb);
    sb_append(&sb, t->summary);
    if (t->nactions) {
        sb_append(&sb, "\n\nActions (pass the name in \"action\"):");
        for (int i = 0; i < t->nactions; i++) {
            const action_def *a = &t->actions[i];
            sb_printf(&sb, "\n- %s", a->name);
            if (a->params && *a->params)
                sb_printf(&sb, " (%s)", a->params);
            sb_printf(&sb, ": %s", a->summary);
        }
    }
    return sb_detach(&sb);
}

cJSON *registry_tools_json(char *err, size_t errlen)
{
    cJSON *tools = cJSON_CreateArray();
    for (int i = 0; i < NTOOLS; i++) {
        const tool_def *t = g_tools[i];
        cJSON *props = cJSON_Parse(t->properties ? t->properties : "{}");
        if (!cJSON_IsObject(props)) {
            snprintf(err, errlen, "tool '%s': malformed properties schema near: %.60s", t->name,
                     cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");
            cJSON_Delete(props);
            cJSON_Delete(tools);
            return NULL;
        }
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        if (t->nactions) {
            cJSON *action = cJSON_CreateObject();
            cJSON_AddStringToObject(action, "type", "string");
            cJSON *en = cJSON_AddArrayToObject(action, "enum");
            for (int k = 0; k < t->nactions; k++)
                cJSON_AddItemToArray(en, cJSON_CreateString(t->actions[k].name));
            cJSON_AddStringToObject(action, "description", "Action to perform (see the tool description).");
            /* "action" first, then the tool's parameters. */
            cJSON *all = cJSON_CreateObject();
            cJSON_AddItemToObject(all, "action", action);
            cJSON *p = props->child;
            while (p) {
                cJSON *next = p->next;
                cJSON_AddItemToObject(all, p->string, cJSON_DetachItemViaPointer(props, p));
                p = next;
            }
            cJSON_Delete(props);
            props = all;
        }
        cJSON_AddItemToObject(schema, "properties", props);
        if (t->nactions) {
            cJSON *req = cJSON_AddArrayToObject(schema, "required");
            cJSON_AddItemToArray(req, cJSON_CreateString("action"));
        }

        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", t->name);
        cJSON_AddStringToObject(tool, "title", t->title);
        char *desc = describe(t);
        cJSON_AddStringToObject(tool, "description", desc);
        free(desc);
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON *ann = cJSON_AddObjectToObject(tool, "annotations");
        cJSON_AddStringToObject(ann, "title", t->title);
        cJSON_AddBoolToObject(ann, "readOnlyHint", (t->hints & TH_READONLY) != 0);
        cJSON_AddBoolToObject(ann, "destructiveHint", (t->hints & TH_DESTRUCTIVE) != 0);
        cJSON_AddBoolToObject(ann, "idempotentHint", (t->hints & TH_IDEMPOTENT) != 0);
        cJSON_AddBoolToObject(ann, "openWorldHint", 0);
        cJSON_AddItemToArray(tools, tool);
    }
    return tools;
}

static const action_def *find_action(const tool_def *t, const char *name)
{
    for (int i = 0; i < t->nactions; i++)
        if (strcmp(t->actions[i].name, name) == 0)
            return &t->actions[i];
    for (int i = 0; i < t->nactions; i++)
        if (_stricmp(t->actions[i].name, name) == 0)
            return &t->actions[i];
    return NULL;
}

static void list_actions(tool_ctx *c, const tool_def *t)
{
    strbuf sb;
    sb_init(&sb);
    for (int i = 0; i < t->nactions; i++)
        sb_printf(&sb, "%s%s", i ? ", " : "", t->actions[i].name);
    fail(c, "valid actions for %s: %s", t->name, sb_str(&sb));
    sb_free(&sb);
}

int registry_execute(tool_ctx *c, const char *tool_name)
{
    const tool_def *t = registry_find(tool_name);
    if (!t)
        return -1;
    c->tool = t;
    action_fn fn = t->direct;
    unsigned flags = t->direct_flags;
    const char *action_name = "";
    if (t->nactions) {
        action_name = arg_s(c, "action");
        if (!action_name || !*action_name) {
            fail(c, "missing required argument 'action'");
            list_actions(c, t);
            stats_record(t->name, "", 0, 0, "missing action", NULL);
            return 0;
        }
        c->action = find_action(t, action_name);
        if (!c->action) {
            fail(c, "unknown action '%s'", action_name);
            list_actions(c, t);
            stats_record(t->name, action_name, 0, 0, "unknown action", NULL);
            return 0;
        }
        fn = c->action->fn;
        flags = c->action->flags;
        action_name = c->action->name;
    }

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    int scoped = 0;
    if (session_prepare(c, flags) == 0) {
        if (session_bridge_ok()) {
            td_scope_begin();
            scoped = 1;
        }
        session_guard g;
        if (session_guard_begin(c, flags, &g) == 0) {
            fn(c);
            if (session_guard_end(c, &g) == 1) {
                LOG_W("%s.%s: repeated without a transaction", t->name, action_name);
                sb_clear(&c->out);
                c->is_error = 0;
                c->error_context[0] = 0;
                td_clear_err();
                if (session_guard_begin(c, flags | AF_NO_TX, &g) == 0) {
                    out(c, "(TIA Portal could not keep this call in one transaction: it was rolled back and repeated "
                           "without one)\n");
                    fn(c);
                    session_guard_end(c, &g);
                }
            }
        }
    }
    session_finish_call(c);
    if (scoped && session_bridge_ok())
        td_scope_end();

    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    if (c->is_error) {
        const char *e = strstr(sb_str(&c->out), "Error: ");
        stats_record(t->name, action_name, 0, ms, e ? e + 7 : sb_str(&c->out), c->error_context);
        LOG_W("%s%s%s failed after %.0f ms", t->name, *action_name ? "." : "", action_name, ms);
    } else {
        stats_record(t->name, action_name, 1, ms, NULL, NULL);
        LOG_I("%s%s%s ok (%.0f ms)", t->name, *action_name ? "." : "", action_name, ms);
    }
    if (c->out.len == 0)
        out(c, "OK\n");
    return 0;
}
