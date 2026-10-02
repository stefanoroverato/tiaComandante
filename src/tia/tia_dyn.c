#include "tia_dyn.h"

#include "clr/clrhost.h"
#include "util/log.h"
#include "util/utf.h"

#include <windows.h>
#include <objbase.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static tc_bridge_api g_api;
static int g_ready;

static __declspec(thread) int t_failed;
static __declspec(thread) char t_err[4096];
static __declspec(thread) char t_err_type[256];

/* ---- callback registry ------------------------------------------------- */

typedef struct cb_entry {
    long long id;
    td_callback_fn fn;
    void *ctx;
} cb_entry;

static SRWLOCK g_cb_lock = SRWLOCK_INIT;
static cb_entry *g_cbs;
static size_t g_ncbs, g_capcbs;
static long long g_next_cb = 1;

static char *dup_cotask(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = CoTaskMemAlloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static int __cdecl native_callback(long long id, const char *args, char **result)
{
    *result = NULL;
    td_callback_fn fn = NULL;
    void *ctx = NULL;
    AcquireSRWLockShared(&g_cb_lock);
    for (size_t i = 0; i < g_ncbs; i++) {
        if (g_cbs[i].id == id) {
            fn = g_cbs[i].fn;
            ctx = g_cbs[i].ctx;
            break;
        }
    }
    ReleaseSRWLockShared(&g_cb_lock);
    if (!fn) {
        LOG_W("bridge callback %lld not registered", id);
        return 1;
    }
    cJSON *a = cJSON_Parse(args ? args : "[]");
    cJSON *res = NULL;
    int rc = fn(ctx, a, &res);
    cJSON_Delete(a);
    if (res) {
        char *s = cJSON_PrintUnformatted(res);
        cJSON_Delete(res);
        if (s) {
            *result = dup_cotask(s);
            cJSON_free(s);
        }
    }
    return rc;
}

long long td_register_callback(td_callback_fn fn, void *ctx)
{
    AcquireSRWLockExclusive(&g_cb_lock);
    if (g_ncbs == g_capcbs) {
        size_t cap = g_capcbs ? g_capcbs * 2 : 16;
        cb_entry *n = realloc(g_cbs, cap * sizeof *n);
        if (!n)
            abort();
        g_cbs = n;
        g_capcbs = cap;
    }
    long long id = g_next_cb++;
    g_cbs[g_ncbs].id = id;
    g_cbs[g_ncbs].fn = fn;
    g_cbs[g_ncbs].ctx = ctx;
    g_ncbs++;
    ReleaseSRWLockExclusive(&g_cb_lock);
    return id;
}

void td_unregister_callback(long long id)
{
    AcquireSRWLockExclusive(&g_cb_lock);
    for (size_t i = 0; i < g_ncbs; i++) {
        if (g_cbs[i].id == id) {
            g_cbs[i] = g_cbs[--g_ncbs];
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_cb_lock);
}

/* ---- errors ------------------------------------------------------------ */

int td_failed(void) { return t_failed; }
const char *td_err(void) { return t_failed ? t_err : ""; }
const char *td_err_type(void) { return t_failed ? t_err_type : ""; }

void td_clear_err(void)
{
    t_failed = 0;
    t_err[0] = 0;
    t_err_type[0] = 0;
}

void td_set_err(const char *type, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t_err, sizeof t_err, fmt, ap);
    va_end(ap);
    snprintf(t_err_type, sizeof t_err_type, "%s", type ? type : "");
    t_failed = 1;
}

static void set_err_from_json(const cJSON *e)
{
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "type"));
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "message"));
    char buf[4096];
    size_t n = (size_t)snprintf(buf, sizeof buf, "%s", msg ? msg : "unknown error");
    const cJSON *inner = cJSON_GetObjectItemCaseSensitive(e, "inner");
    const cJSON *it;
    cJSON_ArrayForEach(it, inner)
    {
        const char *im = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "message"));
        if (im && n < sizeof buf - 1 && !strstr(buf, im))
            n += (size_t)snprintf(buf + n, sizeof buf - n, " -> %s", im);
    }
    td_set_err(type ? type : "Exception", "%s", buf);
}

/* ---- core -------------------------------------------------------------- */

int td_init(const wchar_t *bridge_dll, const wchar_t *openness_dir, char *err, size_t errlen)
{
    if (g_ready)
        return 0;
    if (clr_start_bridge(bridge_dll, openness_dir, native_callback, &g_api, err, errlen) != 0)
        return -1;
    g_ready = 1;
    return 0;
}

int td_ready(void) { return g_ready; }

cJSON *td_request(cJSON *req)
{
    td_clear_err();
    if (!g_ready) {
        cJSON_Delete(req);
        td_set_err("Bridge", ".NET bridge not initialised");
        return NULL;
    }
    char *text = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!text) {
        td_set_err("Bridge", "out of memory");
        return NULL;
    }
    char *resp = NULL;
    g_api.invoke(text, &resp);
    SecureZeroMemory(text, strlen(text)); /* requests may carry passwords (SecureString arguments) */
    cJSON_free(text);
    if (!resp) {
        td_set_err("Bridge", "empty bridge response");
        return NULL;
    }
    cJSON *j = cJSON_Parse(resp);
    CoTaskMemFree(resp);
    if (!j) {
        td_set_err("Bridge", "invalid bridge response");
        return NULL;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ok"))) {
        cJSON *r = cJSON_DetachItemFromObjectCaseSensitive(j, "r");
        cJSON_Delete(j);
        return r ? r : cJSON_CreateNull();
    }
    set_err_from_json(cJSON_GetObjectItemCaseSensitive(j, "err"));
    cJSON_Delete(j);
    return NULL;
}

static cJSON *req_new(const char *op)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "op", op);
    return r;
}

static void req_h(cJSON *r, const char *key, th h)
{
    cJSON_AddNumberToObject(r, key, (double)h);
}

static void req_csv(cJSON *r, const char *key, const char *csv)
{
    if (csv && *csv)
        cJSON_AddStringToObject(r, key, csv);
}

static void req_args(cJSON *r, cJSON *args)
{
    if (args)
        cJSON_AddItemToObject(r, "args", args);
}

static th result_h(cJSON *v)
{
    if (!v)
        return 0;
    th h = tdv_h(v);
    cJSON_Delete(v);
    return h;
}

cJSON *td_get(th h, const char *name)
{
    cJSON *r = req_new("get");
    req_h(r, "h", h);
    cJSON_AddStringToObject(r, "name", name);
    return td_request(r);
}

th td_get_h(th h, const char *name) { return result_h(td_get(h, name)); }

char *td_get_s(th h, const char *name)
{
    cJSON *v = td_get(h, name);
    if (!v)
        return NULL;
    char *s = tc_strdup(tdv_s(v));
    cJSON_Delete(v);
    return s;
}

int td_get_i(th h, const char *name, long long *out)
{
    cJSON *v = td_get(h, name);
    if (!v)
        return -1;
    int rc = 0;
    if (cJSON_IsNumber(v))
        *out = (long long)v->valuedouble;
    else if (cJSON_IsObject(v) && cJSON_GetObjectItemCaseSensitive(v, "$enum"))
        *out = tdv_i(cJSON_GetObjectItemCaseSensitive(v, "value"), 0);
    else
        rc = -1;
    cJSON_Delete(v);
    return rc;
}

int td_get_b(th h, const char *name, int *out)
{
    cJSON *v = td_get(h, name);
    if (!v)
        return -1;
    int rc = cJSON_IsBool(v) ? 0 : -1;
    if (rc == 0)
        *out = cJSON_IsTrue(v);
    cJSON_Delete(v);
    return rc;
}

int td_set(th h, const char *name, cJSON *value)
{
    cJSON *r = req_new("set");
    req_h(r, "h", h);
    cJSON_AddStringToObject(r, "name", name);
    cJSON_AddItemToObject(r, "value", value ? value : cJSON_CreateNull());
    cJSON *res = td_request(r);
    if (!res)
        return -1;
    cJSON_Delete(res);
    return 0;
}

static cJSON *call_ex(th h, const char *method, const char *sig, const char *generic, cJSON *args)
{
    cJSON *r = req_new("call");
    req_h(r, "h", h);
    cJSON_AddStringToObject(r, "name", method);
    req_csv(r, "sig", sig);
    req_csv(r, "generic", generic);
    req_args(r, args);
    return td_request(r);
}

cJSON *td_call(th h, const char *method, cJSON *args) { return call_ex(h, method, NULL, NULL, args); }
cJSON *td_call_sig(th h, const char *method, const char *sig_csv, cJSON *args) { return call_ex(h, method, sig_csv, NULL, args); }
cJSON *td_call_generic(th h, const char *method, const char *generic_csv, cJSON *args) { return call_ex(h, method, NULL, generic_csv, args); }
th td_call_h(th h, const char *method, cJSON *args) { return result_h(td_call(h, method, args)); }

int td_call_v(th h, const char *method, cJSON *args)
{
    cJSON *v = td_call(h, method, args);
    if (!v)
        return -1;
    cJSON_Delete(v);
    return 0;
}

cJSON *td_static(const char *type, const char *name, cJSON *args)
{
    cJSON *r = req_new("static");
    cJSON_AddStringToObject(r, "type", type);
    cJSON_AddStringToObject(r, "name", name);
    req_args(r, args);
    return td_request(r);
}

th td_static_h(const char *type, const char *name, cJSON *args) { return result_h(td_static(type, name, args)); }

th td_new(const char *type, cJSON *args)
{
    cJSON *r = req_new("new");
    cJSON_AddStringToObject(r, "type", type);
    req_args(r, args);
    return result_h(td_request(r));
}

th td_service(th h, const char *type)
{
    cJSON *r = req_new("service");
    req_h(r, "h", h);
    cJSON_AddStringToObject(r, "type", type);
    return result_h(td_request(r));
}

cJSON *td_enum(th h, const char *attrs_csv, int limit)
{
    cJSON *r = req_new("enumerate");
    req_h(r, "h", h);
    req_csv(r, "attrs", attrs_csv);
    if (limit >= 0)
        cJSON_AddNumberToObject(r, "limit", limit);
    return td_request(r);
}

cJSON *td_attrs(th h, const char *attrs_csv)
{
    cJSON *r = req_new("attrs");
    req_h(r, "h", h);
    req_csv(r, "attrs", attrs_csv);
    return td_request(r);
}

int td_is(th h, const char *type)
{
    if (!h)
        return 0;
    cJSON *r = req_new("is");
    req_h(r, "h", h);
    cJSON_AddStringToObject(r, "type", type);
    cJSON *v = td_request(r);
    int res = v && cJSON_IsTrue(v);
    cJSON_Delete(v);
    return res;
}

char *td_typename(th h)
{
    cJSON *r = req_new("type");
    req_h(r, "h", h);
    cJSON *v = td_request(r);
    if (!v)
        return NULL;
    char *s = tc_strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(v, "type")));
    cJSON_Delete(v);
    return s;
}

cJSON *td_members(th h, const char *type)
{
    cJSON *r = req_new("members");
    if (h)
        req_h(r, "h", h);
    else
        cJSON_AddStringToObject(r, "type", type);
    return td_request(r);
}

char *td_tostring(th h)
{
    cJSON *r = req_new("tostring");
    req_h(r, "h", h);
    cJSON *v = td_request(r);
    if (!v)
        return NULL;
    char *s = tc_strdup(cJSON_GetStringValue(v));
    cJSON_Delete(v);
    return s;
}

long long td_subscribe(th h, const char *event, long long cb)
{
    cJSON *r = req_new("subscribe");
    req_h(r, "h", h);
    cJSON_AddStringToObject(r, "name", event);
    cJSON_AddNumberToObject(r, "cb", (double)cb);
    cJSON *v = td_request(r);
    long long sub = v ? tdv_i(v, 0) : 0;
    cJSON_Delete(v);
    return sub;
}

void td_unsubscribe(long long sub)
{
    cJSON *r = req_new("unsubscribe");
    cJSON_AddNumberToObject(r, "sub", (double)sub);
    cJSON_Delete(td_request(r));
}

th td_delegate(const char *type, long long cb)
{
    cJSON *r = req_new("delegate");
    cJSON_AddStringToObject(r, "type", type);
    cJSON_AddNumberToObject(r, "cb", (double)cb);
    return result_h(td_request(r));
}

static void simple_op(const char *op, th h)
{
    cJSON *r = req_new(op);
    if (h)
        req_h(r, "h", h);
    cJSON_Delete(td_request(r));
}

void td_scope_begin(void) { simple_op("scope_begin", 0); }
void td_scope_end(void) { simple_op("scope_end", 0); }
void td_pin(th h) { if (h) simple_op("pin", h); }
void td_unpin(th h) { if (h) simple_op("unpin", h); }
void td_release(th h) { if (h) simple_op("release", h); }

long long td_handle_count(void)
{
    cJSON *v = td_request(req_new("count"));
    long long n = v ? tdv_i(v, -1) : -1;
    cJSON_Delete(v);
    return n;
}

/* ---- builders and value helpers ----------------------------------------- */

static cJSON *tagged(const char *tag, const char *s)
{
    if (!s)
        return cJSON_CreateNull();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, tag, s);
    return o;
}

cJSON *tda(const char *fmt, ...)
{
    cJSON *a = cJSON_CreateArray();
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        cJSON *v = NULL;
        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            v = s ? cJSON_CreateString(s) : cJSON_CreateNull();
            break;
        }
        case 'i': v = cJSON_CreateNumber((double)va_arg(ap, long long)); break;
        case 'I': v = cJSON_CreateNumber((double)va_arg(ap, int)); break;
        case 'd': v = cJSON_CreateNumber(va_arg(ap, double)); break;
        case 'b': v = cJSON_CreateBool(va_arg(ap, int)); break;
        case 'h': {
            th h = va_arg(ap, th);
            if (h) {
                v = cJSON_CreateObject();
                cJSON_AddNumberToObject(v, "$h", (double)h);
            } else {
                v = cJSON_CreateNull();
            }
            break;
        }
        case 'f': v = tagged("$file", va_arg(ap, const char *)); break;
        case 'D': v = tagged("$dir", va_arg(ap, const char *)); break;
        case 't': v = tagged("$type", va_arg(ap, const char *)); break;
        case 'e': {
            const char *type = va_arg(ap, const char *);
            const char *name = va_arg(ap, const char *);
            v = cJSON_CreateObject();
            cJSON_AddStringToObject(v, "$enum", type);
            cJSON_AddStringToObject(v, "name", name);
            break;
        }
        case 'c':
            v = cJSON_CreateObject();
            cJSON_AddNumberToObject(v, "$cb", (double)va_arg(ap, long long));
            break;
        case 'j': {
            cJSON *j = va_arg(ap, cJSON *);
            v = j ? j : cJSON_CreateNull();
            break;
        }
        case 'n': v = cJSON_CreateNull(); break;
        default: break;
        }
        if (v)
            cJSON_AddItemToArray(a, v);
    }
    va_end(ap);
    return a;
}

th tdv_h(const cJSON *v)
{
    if (!cJSON_IsObject(v))
        return 0;
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(v, "$h");
    return cJSON_IsNumber(h) ? (th)h->valuedouble : 0;
}

const char *tdv_s(const cJSON *v)
{
    if (!v)
        return NULL;
    if (cJSON_IsString(v))
        return v->valuestring;
    if (cJSON_IsObject(v)) {
        static const char *keys[] = { "$file", "$dir", "$culture" };
        if (cJSON_GetObjectItemCaseSensitive(v, "$enum"))
            return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(v, "name"));
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            const cJSON *s = cJSON_GetObjectItemCaseSensitive(v, keys[i]);
            if (cJSON_IsString(s))
                return s->valuestring;
        }
    }
    return NULL;
}

long long tdv_i(const cJSON *v, long long def)
{
    if (cJSON_IsNumber(v))
        return (long long)v->valuedouble;
    if (cJSON_IsObject(v) && cJSON_GetObjectItemCaseSensitive(v, "$enum"))
        return tdv_i(cJSON_GetObjectItemCaseSensitive(v, "value"), def);
    if (cJSON_IsBool(v))
        return cJSON_IsTrue(v);
    return def;
}

double tdv_d(const cJSON *v, double def)
{
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

int tdv_b(const cJSON *v, int def)
{
    if (cJSON_IsBool(v))
        return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v))
        return v->valuedouble != 0;
    return def;
}

const char *tdv_type(const cJSON *v)
{
    if (!cJSON_IsObject(v))
        return NULL;
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(v, "$t"));
    return t ? t : cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(v, "$enum"));
}

int tdv_is_err(const cJSON *v)
{
    return cJSON_IsObject(v) && cJSON_GetObjectItemCaseSensitive(v, "$err") != NULL;
}

const cJSON *tdi_a(const cJSON *item, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item, "a"), name);
}

const char *tdi_s(const cJSON *item, const char *name) { return tdv_s(tdi_a(item, name)); }
long long tdi_i(const cJSON *item, const char *name, long long def) { return tdv_i(tdi_a(item, name), def); }
int tdi_b(const cJSON *item, const char *name, int def) { return tdv_b(tdi_a(item, name), def); }

char *tdv_text(const cJSON *v)
{
    char buf[512];
    if (!v || cJSON_IsNull(v))
        return tc_strdup("");
    const char *s = tdv_s(v);
    if (s)
        return tc_strdup(s);
    if (cJSON_IsBool(v))
        return tc_strdup(cJSON_IsTrue(v) ? "true" : "false");
    if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        if (d == (double)(long long)d)
            snprintf(buf, sizeof buf, "%lld", (long long)d);
        else
            snprintf(buf, sizeof buf, "%.15g", d);
        return tc_strdup(buf);
    }
    if (tdv_is_err(v)) {
        snprintf(buf, sizeof buf, "<error: %s>", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(v, "$err")));
        return tc_strdup(buf);
    }
    const char *t = tdv_type(v);
    if (t) {
        snprintf(buf, sizeof buf, "<%s>", t);
        return tc_strdup(buf);
    }
    char *p = cJSON_PrintUnformatted(v);
    char *r = tc_strdup(p);
    cJSON_free(p);
    return r;
}
