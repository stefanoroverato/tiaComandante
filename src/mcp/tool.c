#include "tool.h"

#include "tia/tia_dyn.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ctx_init(tool_ctx *c, const tool_def *tool, const cJSON *args)
{
    memset(c, 0, sizeof *c);
    c->tool = tool;
    c->args = args;
    sb_init(&c->out);
    c->owned = cJSON_CreateArray();
}

void ctx_free(tool_ctx *c)
{
    sb_free(&c->out);
    cJSON_Delete(c->owned);
    c->owned = NULL;
}

const cJSON *arg_get(tool_ctx *c, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(c->args, name);
    return cJSON_IsNull(v) ? NULL : v;
}

int arg_has(tool_ctx *c, const char *name)
{
    return arg_get(c, name) != NULL;
}

const char *arg_s(tool_ctx *c, const char *name)
{
    const cJSON *v = arg_get(c, name);
    if (cJSON_IsString(v))
        return v->valuestring;
    if (cJSON_IsNumber(v)) {
        /* Clients sometimes send names like "1" as numbers: keep the text form. */
        char buf[64];
        if (v->valuedouble == (double)(long long)v->valuedouble)
            snprintf(buf, sizeof buf, "%lld", (long long)v->valuedouble);
        else
            snprintf(buf, sizeof buf, "%.15g", v->valuedouble);
        cJSON *s = cJSON_CreateString(buf);
        cJSON_AddItemToArray(c->owned, s);
        return s->valuestring;
    }
    return NULL;
}

const char *arg_req(tool_ctx *c, const char *name)
{
    const char *s = arg_s(c, name);
    if (!s || !*s) {
        fail(c, "missing required argument '%s'", name);
        return NULL;
    }
    return s;
}

long long arg_i(tool_ctx *c, const char *name, long long def)
{
    const cJSON *v = arg_get(c, name);
    if (cJSON_IsNumber(v))
        return (long long)v->valuedouble;
    if (cJSON_IsString(v) && *v->valuestring) {
        char *end;
        long long n = strtoll(v->valuestring, &end, 10);
        if (*end == 0)
            return n;
    }
    return def;
}

double arg_d(tool_ctx *c, const char *name, double def)
{
    const cJSON *v = arg_get(c, name);
    if (cJSON_IsNumber(v))
        return v->valuedouble;
    if (cJSON_IsString(v) && *v->valuestring) {
        char *end;
        double d = strtod(v->valuestring, &end);
        if (*end == 0)
            return d;
    }
    return def;
}

int arg_b(tool_ctx *c, const char *name, int def)
{
    const cJSON *v = arg_get(c, name);
    if (cJSON_IsBool(v))
        return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v))
        return v->valuedouble != 0;
    if (cJSON_IsString(v)) {
        if (_stricmp(v->valuestring, "true") == 0 || _stricmp(v->valuestring, "yes") == 0 || strcmp(v->valuestring, "1") == 0)
            return 1;
        if (_stricmp(v->valuestring, "false") == 0 || _stricmp(v->valuestring, "no") == 0 || strcmp(v->valuestring, "0") == 0)
            return 0;
    }
    return def;
}

static const cJSON *arg_json(tool_ctx *c, const char *name, int want_array)
{
    const cJSON *v = arg_get(c, name);
    if (want_array ? cJSON_IsArray(v) : cJSON_IsObject(v))
        return v;
    if (cJSON_IsString(v)) {
        const char *s = v->valuestring;
        while (isspace((unsigned char)*s))
            s++;
        if (*s == (want_array ? '[' : '{')) {
            cJSON *p = cJSON_Parse(s);
            if (p && (want_array ? cJSON_IsArray(p) : cJSON_IsObject(p))) {
                cJSON_AddItemToArray(c->owned, p);
                return p;
            }
            cJSON_Delete(p);
        } else if (want_array && *s) {
            /* A single bare value stands for a one-element array. */
            cJSON *a = cJSON_CreateArray();
            cJSON_AddItemToArray(a, cJSON_CreateString(v->valuestring));
            cJSON_AddItemToArray(c->owned, a);
            return a;
        }
    }
    return NULL;
}

const cJSON *arg_arr(tool_ctx *c, const char *name) { return arg_json(c, name, 1); }
const cJSON *arg_obj(tool_ctx *c, const char *name) { return arg_json(c, name, 0); }

void out(tool_ctx *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    char *buf = malloc((size_t)n + 1);
    if (!buf)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_appendn(&c->out, buf, (size_t)n);
    free(buf);
}

void out_raw(tool_ctx *c, const char *text)
{
    sb_append(&c->out, text);
}

int fail(tool_ctx *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *buf = malloc((size_t)(n > 0 ? n : 0) + 1);
    if (buf) {
        va_start(ap, fmt);
        vsnprintf(buf, (size_t)(n > 0 ? n : 0) + 1, fmt, ap);
        va_end(ap);
        if (c->out.len > 0 && c->out.p[c->out.len - 1] != '\n')
            sb_appendc(&c->out, '\n');
        sb_append(&c->out, "Error: ");
        sb_append(&c->out, buf);
        sb_appendc(&c->out, '\n');
        free(buf);
    }
    c->is_error = 1;
    return -1;
}

int fail_td(tool_ctx *c, const char *what)
{
    const char *type = td_err_type();
    const char *dot = strrchr(type, '.');
    return fail(c, "%s: %s%s%s%s", what, td_err(), *type ? " [" : "", dot ? dot + 1 : type, *type ? "]" : "");
}

void progress(tool_ctx *c, double done, double total, const char *msg)
{
    if (c->progress_fn && c->progress_token)
        c->progress_fn(c, done, total, msg);
}

int cancelled(tool_ctx *c)
{
    return c->cancel && *c->cancel;
}

int confirmed(tool_ctx *c, const char *phrase)
{
    const char *s = arg_s(c, "confirm");
    if (!s)
        return 0;
    size_t n = strlen(phrase);
    for (; *s; s++)
        if (_strnicmp(s, phrase, n) == 0)
            return 1;
    return 0;
}
