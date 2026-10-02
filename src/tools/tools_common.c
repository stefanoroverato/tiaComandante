#include "tools.h"

#include "tia/session.h"
#include "tia/tia_dyn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *item_text(const cJSON *item, const char *attr)
{
    return tdv_text(tdi_a(item, attr));
}

const char *short_type(const cJSON *v)
{
    const char *t = tdv_type(v);
    if (!t)
        return "?";
    const char *dot = strrchr(t, '.');
    return dot ? dot + 1 : t;
}

const char *item_attrs(sw_container cont)
{
    switch (cont) {
    case SWC_BLOCKS: return "Name,Number,ProgrammingLanguage";
    case SWC_TYPES: return "Name,IsConsistent";
    default: return "Name";
    }
}

void print_item(tool_ctx *c, sw_container cont, const cJSON *item, const char *folder)
{
    const char *name = tdi_s(item, "Name");
    const char *type = short_type(item);
    out(c, "%s%s%s  [type=%s", folder ? folder : "", folder && *folder ? "/" : "", name ? name : "?", type);
    switch (cont) {
    case SWC_BLOCKS: {
        char *num = item_text(item, "Number");
        out(c, ", num=%s, lang=%s", num, tdi_s(item, "ProgrammingLanguage") ? tdi_s(item, "ProgrammingLanguage") : "?");
        free(num);
        break;
    }
    case SWC_TAG_TABLES: {
        long long n = -1;
        th tags = td_get_h(tdv_h(item), "Tags");
        if (tags && td_get_i(tags, "Count", &n) == 0)
            out(c, ", tags=%lld", n);
        break;
    }
    case SWC_TYPES:
        out(c, ", consistent=%s", tdi_b(item, "IsConsistent", 1) ? "true" : "false");
        break;
    case SWC_WATCH_TABLES: {
        long long n = -1;
        th entries = td_get_h(tdv_h(item), "Entries");
        if (entries && td_get_i(entries, "Count", &n) == 0)
            out(c, ", entries=%lld", n);
        out(c, ", force=%s", strstr(type, "Force") ? "true" : "false");
        break;
    }
    default:
        break;
    }
    td_clear_err();
    out(c, "]\n");
}

/* ---- compilation ---------------------------------------------------------------- */

typedef struct msg_ctx {
    tool_ctx *c;
    int errors_only;
    int printed;
    int limit;
} msg_ctx;

static void walk_messages(msg_ctx *m, th messages, const char *prefix, int depth)
{
    if (!messages || depth > 12)
        return;
    cJSON *list = td_enum(messages, "Path,Description,State,ErrorCount,WarningCount", -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        const char *path = tdi_s(it, "Path");
        const char *desc = tdi_s(it, "Description");
        const char *state = tdi_s(it, "State");
        char full[1024];
        snprintf(full, sizeof full, "%s%s%s", prefix, *prefix && path && *path ? " > " : "", path ? path : "");
        th sub = td_get_h(tdv_h(it), "Messages");
        cJSON *peek = sub ? td_enum(sub, NULL, 1) : NULL;
        int leaf = cJSON_GetArraySize(peek) == 0;
        cJSON_Delete(peek);
        int relevant = state && (strcmp(state, "Error") == 0 || (!m->errors_only && strcmp(state, "Warning") == 0));
        if (desc && *desc && (relevant || (!m->errors_only && leaf))) {
            if (m->printed < m->limit)
                out(m->c, "[%s] %s: %s\n", state ? state : "?", full, desc);
            m->printed++;
        }
        if (!leaf)
            walk_messages(m, sub, full, depth + 1);
    }
    cJSON_Delete(list);
}

static int report_compile(tool_ctx *c, th result, int errors_only)
{
    cJSON *a = td_attrs(result, "State,ErrorCount,WarningCount");
    const char *state = tdv_s(cJSON_GetObjectItemCaseSensitive(a, "State"));
    long long errs = tdv_i(cJSON_GetObjectItemCaseSensitive(a, "ErrorCount"), 0);
    long long warns = tdv_i(cJSON_GetObjectItemCaseSensitive(a, "WarningCount"), 0);
    out(c, "Compile result: %s (errors: %lld, warnings: %lld)\n", state ? state : "?", errs, warns);
    cJSON_Delete(a);
    msg_ctx m = { c, errors_only, 0, (int)arg_i(c, "limit", 200) };
    walk_messages(&m, td_get_h(result, "Messages"), "", 0);
    if (m.printed > m.limit)
        out(c, "... %d more message(s) not shown (raise limit)\n", m.printed - m.limit);
    return 0;
}

int compile_object(tool_ctx *c, th obj, int errors_only)
{
    th comp = td_service(obj, "Siemens.Engineering.Compiler.ICompilable");
    if (!comp)
        return td_failed() ? fail_td(c, "compiler service unavailable") : fail(c, "this object cannot be compiled");
    progress(c, 0, 0, "compiling");
    th result = session_compile(comp);
    if (!result)
        return fail_td(c, "compilation failed");
    return report_compile(c, result, errors_only);
}

