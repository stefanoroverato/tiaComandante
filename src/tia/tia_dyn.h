/* tia_dyn - C API over the reflection bridge.
 *
 * Every .NET object lives in the bridge's handle table and is referred to by a
 * th (0 = null). Values cross the boundary as cJSON "wire" values:
 *   primitives        -> JSON number/string/bool/null
 *   enum              -> {"$enum": "Full.Type", "name": "SCL", "value": 3}
 *   FileInfo / DirectoryInfo / CultureInfo -> {"$file": path} / {"$dir": path} / {"$culture": name}
 *   any other object  -> {"$h": id, "$t": "Full.Type"}
 *
 * Functions returning cJSON* give ownership to the caller; NULL means failure
 * (see td_err()). A .NET null result is returned as a cJSON null item.
 * Functions taking cJSON* args take ownership of them.
 */
#ifndef TC_TIA_DYN_H
#define TC_TIA_DYN_H

#include "cJSON.h"

#include <wchar.h>

typedef long long th;

/* Callback for .NET events/delegates: args is a JSON array of wire values
   (handles valid only during the call). Set *result to a wire value (owned by
   the bridge glue) for non-void delegates. Runs on arbitrary threads. */
typedef int (*td_callback_fn)(void *ctx, const cJSON *args, cJSON **result);

int td_init(const wchar_t *bridge_dll, const wchar_t *openness_dir, char *err, size_t errlen);
int td_ready(void);

/* Error state of the last td_* call on this thread. */
int td_failed(void);
const char *td_err(void);
const char *td_err_type(void);
void td_clear_err(void);
void td_set_err(const char *type, const char *fmt, ...);

/* Raw request (takes ownership of req). */
cJSON *td_request(cJSON *req);

cJSON *td_get(th h, const char *name);
th td_get_h(th h, const char *name);
char *td_get_s(th h, const char *name);              /* malloc'ed, NULL if null/error */
int td_get_i(th h, const char *name, long long *out); /* 0 = ok */
int td_get_b(th h, const char *name, int *out);       /* 0 = ok */
int td_set(th h, const char *name, cJSON *value);     /* 0 = ok */

cJSON *td_call(th h, const char *method, cJSON *args);
cJSON *td_call_sig(th h, const char *method, const char *sig_csv, cJSON *args);
cJSON *td_call_generic(th h, const char *method, const char *generic_csv, cJSON *args);
th td_call_h(th h, const char *method, cJSON *args);
int td_call_v(th h, const char *method, cJSON *args); /* discards result, 0 = ok */
cJSON *td_static(const char *type, const char *name, cJSON *args);
th td_static_h(const char *type, const char *name, cJSON *args);
th td_new(const char *type, cJSON *args);
th td_service(th h, const char *type);                /* 0 if the service is not available */

/* Enumerate an IEnumerable; with attrs_csv each item carries "a": {attr: value}. */
cJSON *td_enum(th h, const char *attrs_csv, int limit);
cJSON *td_attrs(th h, const char *attrs_csv);
int td_is(th h, const char *type);
char *td_typename(th h);
cJSON *td_members(th h, const char *type);
char *td_tostring(th h);

long long td_register_callback(td_callback_fn fn, void *ctx);
void td_unregister_callback(long long id);
long long td_subscribe(th h, const char *event, long long cb);
void td_unsubscribe(long long sub);
th td_delegate(const char *type, long long cb);

void td_scope_begin(void);
void td_scope_end(void);
void td_pin(th h);
void td_unpin(th h);
void td_release(th h);
long long td_handle_count(void);

/* Argument list builder. Format characters (spaces ignored):
 *   s string (NULL -> null)   i long long   I int   d double   b bool (int)
 *   h handle (0 -> null)      f file path   D directory path   t type name
 *   e enum: type name, member name        c callback id (long long)
 *   j cJSON* (ownership taken)            n null
 */
cJSON *tda(const char *fmt, ...);

/* Wire value helpers. */
th tdv_h(const cJSON *v);                 /* handle or 0 */
const char *tdv_s(const cJSON *v);        /* string, enum name, file/dir path, culture; else NULL */
long long tdv_i(const cJSON *v, long long def);
double tdv_d(const cJSON *v, double def);
int tdv_b(const cJSON *v, int def);
const char *tdv_type(const cJSON *v);     /* "$t" of a handle or "$enum" of an enum */
int tdv_is_err(const cJSON *v);           /* attribute read failure marker {"$err": ...} */

/* Helpers for td_enum items: attribute "a"[name]. */
const cJSON *tdi_a(const cJSON *item, const char *name);
const char *tdi_s(const cJSON *item, const char *name);
long long tdi_i(const cJSON *item, const char *name, long long def);
int tdi_b(const cJSON *item, const char *name, int def);

/* Renders a wire value as text (strings unquoted, enums by name, handles as <Type>). Caller frees. */
char *tdv_text(const cJSON *v);

#endif
