/* live - live PLC data over S7CommPlus.
 *
 * Uses S7CommPlusDriver (third_party/s7commplus, LGPL-3.0), loaded into the
 * .NET bridge on first use, with OpenSSL 3 for the TLS session. One session at
 * a time, used only from the worker thread. Read-only: values are never
 * written and the operating state is never changed.
 */
#ifndef TC_LIVE_H
#define TC_LIVE_H

#include "mcp/tool.h"
#include "tia/tia_dyn.h"
#include "util/strbuf.h"

typedef struct live_ident {
    char raw[160];     /* session identification, e.g. "1;6ES7 511-1AK02-0AB0 ;V2.9" */
    char order[64];    /* "6ES7 511-1AK02-0AB0" */
    char firmware[32]; /* "V2.9" */
    int simulated;     /* PLCSIM ("6ES7 SIM-...") */
} live_ident;

/* First driver file missing next to the server, or NULL. */
const char *live_missing_file(void);
/* Driver and OpenSSL files with their versions, one line each. */
void live_describe_driver(strbuf *sb);

int live_connected(void);
const char *live_host(void);
const live_ident *live_identity(void);
long long live_since(void);      /* unix time of the connection */
int live_used_credential(void);  /* the session was opened with a stored PLC credential */

/* Opens a session to host (closing any other). The password / PLC user comes from the
   Windows Credential Manager (key <host>, then "*") when stored. Prints a summary. */
int live_connect(tool_ctx *c, const char *host, int timeout_ms);
void live_disconnect(void);
/* Fails the call unless a session is open. */
int live_require(tool_ctx *c);

/* Effective protection level 1..4 (1 = full access), or -1 (a lost session is closed). */
int live_access_level(tool_ctx *c);
const char *live_access_name(int level);

/* Alarms with texts in language lcid, at most max listed. The configured alarms are
   optional information: a failure is printed, not returned. */
int live_print_active_alarms(tool_ctx *c, int lcid, int max);
void live_print_configured_alarms(tool_ctx *c, int lcid, int max);

/* Variables the PLC exposes (browsed once per session, refresh to read again). */
typedef struct live_var {
    char *name;   /* "DB.member", "MArea.Tag", ... */
    char *access; /* access sequence: hex ids separated by '.' */
    unsigned sdt; /* S7CommPlus soft data type */
} live_var;
int live_browse(tool_ctx *c, int refresh, const live_var **vars, int *n, double *ms);
/* TIA data type name of a soft data type ("Int", "Real", ...). */
const char *live_type_name(unsigned sdt);

/* One read request: add variables or symbols, execute, then use vals[i]. */
typedef struct live_value {
    char *text;     /* formatted value (TIA notation), NULL when there is no value */
    int good;       /* quality GOOD */
    unsigned sdt;
    char note[200]; /* why there is no value */
} live_value;

typedef struct live_read {
    th list;     /* List<PlcTag> with the resolved tags */
    int *slot;   /* request index of each list entry */
    int nslots;
    live_value *vals;
    int n, cap;
} live_read;

void live_read_init(live_read *r);
int live_read_add_var(live_read *r, const live_var *v);
/* Symbol in TIA notation: "\"DB\".member", "\"DB\".arr[2]", "\"Tag\"". */
int live_read_add_symbol(live_read *r, const char *symbol);
int live_read_exec(tool_ctx *c, live_read *r);
void live_read_free(live_read *r);

#endif
