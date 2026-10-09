/* live_data: live PLC values and CPU information over S7CommPlus (read-only). */
#include "tools.h"

#include "app/credentials.h"
#include "tia/live.h"
#include "tia/online.h"
#include "tia/session.h"
#include "tia/simaticml.h"
#include "tia/tia_dyn.h"
#include "tia/tia_nav.h"
#include "tia/tia_sw.h"
#include "util/strbuf.h"
#include "util/utf.h"

#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NO_PASSWORD_ARGS                                                                                               \
    "passwords are not accepted as tool arguments (they would pass through the chat): store the PLC password once "   \
    "with live_data action=set_credential host=<PLC IP> (Windows dialog); connect then uses it automatically"

static int reject_password(tool_ctx *c)
{
    return arg_has(c, "password") ? fail(c, "%s", NO_PASSWORD_ARGS) : 0;
}

static int timeout_arg(tool_ctx *c)
{
    long long t = arg_i(c, "timeoutMs", 5000);
    return (int)(t < 500 ? 500 : t > 60000 ? 60000 : t);
}

/* Probes an open session without failing the call; a lost session is closed. */
static int session_alive(tool_ctx *c)
{
    if (!live_connected())
        return 0;
    tool_ctx probe;
    ctx_init(&probe, c->tool, NULL);
    int level = live_access_level(&probe);
    ctx_free(&probe);
    if (level > 0)
        return 1;
    live_disconnect();
    return 0;
}

/* "\"Name\"" or Name -> Name */
static void unquote(const char *in, char *out, size_t cap)
{
    while (isspace((unsigned char)*in))
        in++;
    size_t n = strlen(in);
    while (n && isspace((unsigned char)in[n - 1]))
        n--;
    if (n >= 2 && in[0] == '"' && in[n - 1] == '"')
        in++, n -= 2;
    snprintf(out, cap, "%.*s", (int)n, in);
}

/* Root of a browsed name: "DB.a.b" -> "DB", "MArea.Tag" -> "MArea". */
static size_t root_len(const char *name)
{
    return strcspn(name, ".[");
}

static const char *root_alias(const char *r)
{
    if (_stricmp(r, "I") == 0 || _stricmp(r, "inputs") == 0)
        return "IArea";
    if (_stricmp(r, "Q") == 0 || _stricmp(r, "outputs") == 0)
        return "QArea";
    if (_stricmp(r, "M") == 0 || _stricmp(r, "memory") == 0 || _stricmp(r, "merker") == 0)
        return "MArea";
    return r;
}

static int same_root(const char *name, const char *root)
{
    size_t n = root_len(name);
    return strlen(root) == n && _strnicmp(name, root, n) == 0;
}

/* "6ES7 511-1AK02-0AB0" == "6ES7511-1AK02-0AB0" */
static int same_order(const char *a, const char *b)
{
    for (;;) {
        while (*a == ' ')
            a++;
        while (*b == ' ')
            b++;
        if (!*a || !*b)
            return !*a && !*b;
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
        a++, b++;
    }
}

/* Makes sure the session reaches the PLC of a project device: host, else the device's configured IP (an open
   session to another address is replaced), and refuses a session whose CPU has another order number. */
static int ensure_device_session(tool_ctx *c, const nav_plc *plc)
{
    char ip[64];
    on_device_ip(plc, ip, sizeof ip, NULL);
    const char *host = arg_s(c, "host");
    if (!host || !*host)
        host = ip;
    int alive = session_alive(c);
    if (!alive && !*host)
        return fail(c, "no live-data session and %s has no configured IP: connect first (live_data action=connect "
                       "host=<PLC IP>) or pass host", plc->device_name);
    if (*host && (!alive || _stricmp(host, live_host()) != 0)) {
        out(c, "(connecting to %s for %s)\n", host, plc->device_name);
        if (live_connect(c, host, timeout_arg(c)) != 0)
            return -1;
    }
    const live_ident *id = live_identity();
    char *order = td_get_s(plc->cpu, "OrderNumber");
    td_clear_err();
    if (id->simulated)
        out(c, "(the connected PLC is a simulation: its order number is not compared with %s%s%s)\n", plc->device_name,
            order ? " " : "", order ? order : "");
    else if (order && *order && id->order[0] && !same_order(order, id->order)) {
        fail(c, "the PLC at %s is a %s, but %s is configured as %s: this is not the PLC of the project device. Connect "
                "to the right PLC (live_data action=connect host=<IP>).", live_host(), id->order, plc->device_name, order);
        free(order);
        return -1;
    }
    free(order);
    if (*ip && strcmp(ip, live_host()) != 0)
        out(c, "(the session goes to %s; the configured IP of %s is %s)\n", live_host(), plc->device_name, ip);
    return 0;
}

static void print_value(tool_ctx *c, const live_value *v)
{
    if (v->text)
        out(c, " = %s", v->text);
    else
        out(c, ": %s", v->note[0] ? v->note : "no value");
}

/* ---- session ------------------------------------------------------------------------------ */

static int a_connect(tool_ctx *c)
{
    if (reject_password(c) != 0)
        return -1;
    const char *host = arg_s(c, "host");
    char ip[64] = "";
    if ((!host || !*host) && arg_s(c, "deviceName")) {
        if (session_prepare(c, AF_PROJECT) != 0)
            return -1;
        nav_plc plc;
        if (sw_plc(c, &plc) != 0)
            return -1;
        on_device_ip(&plc, ip, sizeof ip, NULL);
        if (!*ip)
            return fail(c, "%s has no configured IP address: pass host", plc.device_name);
        host = ip;
    }
    if (!host || !*host)
        return fail(c, "missing required argument 'host' (PLC IP address)");
    if (live_connected() && _stricmp(live_host(), host) == 0 && !arg_b(c, "reconnect", 0) && session_alive(c)) {
        const live_ident *id = live_identity();
        out(c, "Already connected to %s (%s %s). reconnect=true opens a new session.\n", host, id->order, id->firmware);
        return 0;
    }
    return live_connect(c, host, timeout_arg(c));
}

static int a_disconnect(tool_ctx *c)
{
    if (!live_connected()) {
        out(c, "No live-data session.\n");
        return 0;
    }
    char host[64];
    snprintf(host, sizeof host, "%s", live_host());
    live_disconnect();
    out(c, "Live-data session to %s closed.\n", host);
    return 0;
}

static int a_get_status(tool_ctx *c)
{
    strbuf sb;
    sb_init(&sb);
    live_describe_driver(&sb);
    const char *missing = live_missing_file();
    out(c, "Live data: S7CommPlus over TLS with S7CommPlusDriver (LGPL-3.0) and OpenSSL 3, read-only. %s\nFiles:\n%s",
        missing ? "NOT INSTALLED" : "ready", sb_str(&sb));
    sb_free(&sb);
    if (live_connected() && !session_alive(c))
        out(c, "The live-data session was lost and has been closed.\n");
    if (!live_connected()) {
        out(c, "Session: none (live_data action=connect host=<PLC IP>).\n");
        return 0;
    }
    const live_ident *id = live_identity();
    long long age = (long long)time(NULL) - live_since();
    out(c, "Session: %s, open for %lld min %lld s\n", live_host(), age / 60, age % 60);
    out(c, "PLC: %s, firmware %s%s\n", id->order[0] ? id->order : "?", id->firmware[0] ? id->firmware : "?",
        id->simulated ? " (simulation)" : "");
    tool_ctx probe;
    ctx_init(&probe, c->tool, NULL);
    int level = live_access_level(&probe);
    ctx_free(&probe);
    out(c, "Access level: %s%s\n", live_access_name(level),
        live_used_credential() ? " (a stored PLC credential was offered: it is used when the PLC asks for a login)" : "");
    out(c, "Note: the driver does not verify the PLC certificate (TLS without server authentication).\n");
    return 0;
}

/* ---- credentials ------------------------------------------------------------------------- */

static int a_set_credential(tool_ctx *c)
{
    if (reject_password(c) != 0)
        return -1;
    const char *host = arg_req(c, "host");
    if (!host)
        return -1;
    int global = arg_b(c, "global", 0);
    char msg[800], err[256];
    snprintf(msg, sizeof msg,
             "PLC credentials for %s (live data, online, download)\n\nAccess-level password only: user name \"-\".\nPLC "
             "user management: the PLC user (%s).",
             strcmp(host, "*") == 0 ? "any PLC" : host, global ? "global user" : "project user");
    int rc = cred_prompt_store(CRED_PLC, host, global, msg, err, sizeof err);
    if (rc == 1)
        return fail(c, "the user cancelled the credential dialog");
    if (rc != 0)
        return fail(c, "%s", err);
    out(c, "Credential stored in the Windows Credential Manager as tiaComandante/plc/%s (the password was entered only in "
           "the Windows dialog). It is used by live_data connect and by go_online, download and upload.%s\n",
        host, live_connected() && _stricmp(live_host(), host) == 0 ? " Connect again to use it in the live-data session." : "");
    return 0;
}

static int a_delete_credential(tool_ctx *c)
{
    const char *host = arg_req(c, "host");
    if (!host)
        return -1;
    if (cred_delete(CRED_PLC, host) != 0)
        return fail(c, "no stored credential tiaComandante/plc/%s", host);
    out(c, "Credential tiaComandante/plc/%s deleted.\n", host);
    return 0;
}

static int a_list_credentials(tool_ctx *c)
{
    strbuf sb;
    sb_init(&sb);
    cred_list(&sb);
    int n = 0;
    out(c, "Stored PLC credentials (Windows Credential Manager, secrets never shown; key = PLC IP, * = any PLC):\n");
    for (const char *p = sb_str(&sb); *p;) {
        size_t len = strcspn(p, "\n");
        if (strncmp(p, "plc/", 4) == 0) {
            out(c, "  %.*s\n", (int)len, p);
            n++;
        }
        p += len + (p[len] == '\n');
    }
    if (!n)
        out(c, "  (none)\n");
    sb_free(&sb);
    return 0;
}

/* ---- browsing and DBs -------------------------------------------------------------------- */

typedef struct root_count {
    char name[256];
    int n;
} root_count;

static int a_list_readable(tool_ctx *c)
{
    const live_var *v;
    int n;
    double ms;
    if (live_browse(c, arg_b(c, "refresh", 0), &v, &n, &ms) != 0)
        return -1;
    const char *root_arg = arg_s(c, "root");
    if (!root_arg || !*root_arg) {
        root_count *roots = NULL;
        int nroots = 0;
        for (int i = 0; i < n; i++) {
            size_t len = root_len(v[i].name);
            int j = 0;
            while (j < nroots && !(strlen(roots[j].name) == len && strncmp(roots[j].name, v[i].name, len) == 0))
                j++;
            if (j == nroots) {
                root_count *r = realloc(roots, (size_t)(nroots + 1) * sizeof *r);
                if (!r)
                    abort();
                roots = r;
                snprintf(roots[nroots].name, sizeof roots[nroots].name, "%.*s", (int)len, v[i].name);
                roots[nroots++].n = 0;
            }
            roots[j].n++;
        }
        out(c, "Readable on %s: %d value(s) in %d root(s) (browsed in %.0f ms):\n", live_host(), n, nroots, ms);
        for (int j = 0; j < nroots; j++)
            out(c, "  %s: %d\n", roots[j].name, roots[j].n);
        free(roots);
        out(c, "IArea/QArea/MArea are the PLC tags (inputs, outputs, memory); the other roots are data blocks. Pass "
               "root=<name> for the values of one root. A DB or tag missing here is not in the PLC or not accessible "
               "from HMI/OPC UA.\n");
        return 0;
    }
    char root[256];
    unquote(root_arg, root, sizeof root);
    const char *want = root_alias(root);
    int limit = (int)arg_i(c, "limit", 200), shown = 0, total = 0;
    for (int i = 0; i < n; i++) {
        if (!same_root(v[i].name, want))
            continue;
        if (total++ == 0)
            out(c, "%s on %s:\n", want, live_host());
        if (shown < limit) {
            out(c, "  %s  [%s]\n", v[i].name, live_type_name(v[i].sdt));
            shown++;
        }
    }
    if (!total)
        return fail(c, "'%s' is not readable on %s: not in the PLC, not accessible from HMI/OPC UA, or only in load "
                       "memory. live_data action=list_readable without root lists the roots.", root, live_host());
    out(c, "%d value(s)%s.\n", total, shown < total ? " (raise limit to see all)" : "");
    return 0;
}

/* Member argument relative to the DB: "speed", "dbHw.speed" or "\"dbHw\".speed" -> "speed". */
static void member_rel(const char *db, const char *m, char *out_, size_t cap)
{
    while (isspace((unsigned char)*m))
        m++;
    size_t dl = strlen(db);
    if (m[0] == '"' && _strnicmp(m + 1, db, dl) == 0 && m[1 + dl] == '"' && m[2 + dl] == '.')
        m += dl + 3;
    else if (_strnicmp(m, db, dl) == 0 && m[dl] == '.')
        m += dl + 1;
    snprintf(out_, cap, "%s", m);
}

static int member_match(const char *rel, const char *m)
{
    size_t n = strlen(m);
    return _strnicmp(rel, m, n) == 0 && (rel[n] == 0 || rel[n] == '.' || rel[n] == '[');
}

static int a_read_db(tool_ctx *c)
{
    const char *db_arg = arg_req(c, "dbName");
    if (!db_arg)
        return -1;
    char db[256];
    unquote(db_arg, db, sizeof db);
    const live_var *v;
    int n;
    if (live_browse(c, arg_b(c, "refresh", 0), &v, &n, NULL) != 0)
        return -1;
    const cJSON *members = arg_arr(c, "members");
    int nm = cJSON_GetArraySize(members);
    char (*rel)[256] = nm ? calloc((size_t)nm, sizeof *rel) : NULL;
    int *hits = nm ? calloc((size_t)nm, sizeof *hits) : NULL;
    for (int k = 0; k < nm; k++) {
        const char *m = cJSON_GetStringValue(cJSON_GetArrayItem(members, k));
        member_rel(db, m ? m : "", rel[k], sizeof rel[k]);
    }
    int limit = (int)arg_i(c, "limit", 1000);
    live_read r;
    live_read_init(&r);
    int *var_of = calloc((size_t)(n ? n : 1), sizeof *var_of);
    int found = 0, skipped = 0;
    char dbname[256] = "";
    for (int i = 0; i < n; i++) {
        if (!same_root(v[i].name, db))
            continue;
        found++;
        if (!*dbname)
            snprintf(dbname, sizeof dbname, "%.*s", (int)root_len(v[i].name), v[i].name);
        const char *relname = v[i].name + root_len(v[i].name);
        if (*relname == '.')
            relname++;
        int want = nm == 0;
        for (int k = 0; k < nm; k++)
            if (member_match(relname, rel[k])) {
                hits[k]++;
                want = 1;
            }
        if (!want)
            continue;
        if (r.n >= limit) {
            skipped++;
            continue;
        }
        var_of[live_read_add_var(&r, &v[i])] = i;
    }
    int rc = 0;
    if (!found) {
        rc = fail(c, "DB '%s' is not readable on %s: not in the PLC, not accessible from HMI/OPC UA, or only in load "
                     "memory. live_data action=list_readable lists the readable DBs.", db, live_host());
    } else if (live_read_exec(c, &r) == 0) {
        out(c, "DB %s on %s (%d value(s)):\n", dbname, live_host(), r.n);
        for (int j = 0; j < r.n; j++) {
            const live_var *lv = &v[var_of[j]];
            const char *relname = lv->name + root_len(lv->name);
            out(c, "  %s", *relname == '.' ? relname + 1 : relname);
            print_value(c, &r.vals[j]);
            out(c, "  [%s]\n", live_type_name(lv->sdt));
        }
        if (skipped)
            out(c, "%d more value(s) not read: raise limit or pass members.\n", skipped);
        for (int k = 0; k < nm; k++)
            if (!hits[k])
                out(c, "Member not found: %s\n", rel[k]);
    } else {
        rc = -1;
    }
    live_read_free(&r);
    free(var_of);
    free(rel);
    free(hits);
    return rc;
}

/* ---- project tables ---------------------------------------------------------------------- */

static int filter_match(const cJSON *filter, const char *name, const char *addr, int *hit)
{
    if (!filter)
        return 1;
    int k = 0, any = 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, filter)
    {
        char want[256];
        unquote(cJSON_GetStringValue(f) ? cJSON_GetStringValue(f) : "", want, sizeof want);
        if (_stricmp(want, name) == 0 || (addr && *addr && _stricmp(want, addr) == 0)) {
            hit[k] = 1;
            any = 1;
        }
        k++;
    }
    return any;
}

static int a_read_tag_table(tool_ctx *c)
{
    nav_plc plc;
    sw_found t;
    if (sw_plc(c, &plc) != 0)
        return -1;
    const char *table = arg_req(c, "tagTableName");
    if (!table)
        return -1;
    if (sw_find(c, plc.software, SWC_TAG_TABLES, table, &t) != 0)
        return -1;
    if (ensure_device_session(c, &plc) != 0)
        return -1;
    cJSON *tags = td_enum(td_get_h(t.item, "Tags"), "Name,DataTypeName,LogicalAddress", -1);
    if (!tags)
        return fail_td(c, "reading the tag table failed");
    const cJSON *filter = arg_arr(c, "tags");
    int nf = cJSON_GetArraySize(filter);
    int *hit = calloc((size_t)(nf ? nf : 1), sizeof *hit);
    int ntags = cJSON_GetArraySize(tags);
    const cJSON **item_of = calloc((size_t)(ntags ? ntags : 1), sizeof *item_of);
    live_read r;
    live_read_init(&r);
    const cJSON *it;
    cJSON_ArrayForEach(it, tags)
    {
        const char *name = tdi_s(it, "Name");
        if (!name || !filter_match(filter, name, tdi_s(it, "LogicalAddress"), hit))
            continue;
        char sym[512];
        snprintf(sym, sizeof sym, "\"%s\"", name);
        item_of[live_read_add_symbol(&r, sym)] = it;
    }
    int rc = live_read_exec(c, &r);
    if (rc == 0) {
        out(c, "Tag table %s%s%s of %s, live from %s (%d tag(s)):\n", t.folder, *t.folder ? "/" : "", t.name,
            plc.device_name, live_host(), r.n);
        for (int j = 0; j < r.n; j++) {
            const char *addr = tdi_s(item_of[j], "LogicalAddress");
            out(c, "  %s (%s%s%s)", tdi_s(item_of[j], "Name"), addr && *addr ? addr : "", addr && *addr ? ", " : "",
                tdi_s(item_of[j], "DataTypeName") ? tdi_s(item_of[j], "DataTypeName") : "?");
            print_value(c, &r.vals[j]);
            out(c, "\n");
        }
        for (int k = 0; k < nf; k++)
            if (!hit[k])
                out(c, "Not in this table: %s (absolute addresses are read through the tag that uses them)\n",
                    cJSON_GetStringValue(cJSON_GetArrayItem(filter, k)));
    }
    live_read_free(&r);
    free(item_of);
    free(hit);
    cJSON_Delete(tags);
    return rc;
}

/* Watch-table operand to a driver symbol: Tag -> "Tag", DB.member -> "DB".member, quoted stays. NULL for
   operands the symbolic access cannot read (absolute addresses, peripheral access, bit/byte slices). */
static const char *operand_symbol(const char *in, char *out_, size_t cap, const char **why)
{
    while (isspace((unsigned char)*in))
        in++;
    if (*in == '%') {
        *why = "absolute address: only symbolic access is supported (use the tag name)";
        return NULL;
    }
    if (strstr(in, ":P")) {
        *why = "peripheral access (:P) is not readable through symbolic access";
        return NULL;
    }
    if (strstr(in, ".%")) {
        *why = "slice access (.%X, .%B, ...) is not supported";
        return NULL;
    }
    if (*in == '"') {
        snprintf(out_, cap, "%s", in);
        return out_;
    }
    const char *dot = strchr(in, '.');
    if (dot)
        snprintf(out_, cap, "\"%.*s\"%s", (int)(dot - in), in, dot);
    else
        snprintf(out_, cap, "\"%s\"", in);
    return out_;
}

typedef struct operand {
    char text[512];
    const char *why;
    int slot; /* -1 = not read */
} operand;

static int read_operands(tool_ctx *c, operand *ops, int n, const char *title)
{
    live_read r;
    live_read_init(&r);
    for (int i = 0; i < n; i++) {
        char sym[512];
        ops[i].slot = -1;
        if (operand_symbol(ops[i].text, sym, sizeof sym, &ops[i].why))
            ops[i].slot = live_read_add_symbol(&r, sym);
    }
    int rc = live_read_exec(c, &r);
    if (rc == 0) {
        out(c, "%s, live from %s (%d operand(s)):\n", title, live_host(), n);
        for (int i = 0; i < n; i++) {
            out(c, "  %s", ops[i].text);
            if (ops[i].slot >= 0) {
                print_value(c, &r.vals[ops[i].slot]);
                if (r.vals[ops[i].slot].text)
                    out(c, "  [%s]", live_type_name(r.vals[ops[i].slot].sdt));
            } else {
                out(c, ": %s", ops[i].why);
            }
            out(c, "\n");
        }
    }
    live_read_free(&r);
    return rc;
}

static int a_read_watch_table(tool_ctx *c)
{
    const cJSON *symbols = arg_arr(c, "symbols");
    const char *table = arg_s(c, "watchTableName");
    if (symbols && (table || arg_s(c, "deviceName")))
        return fail(c, "pass EITHER symbols[] OR deviceName + watchTableName, not both");
    if (symbols) {
        if (live_require(c) != 0)
            return -1;
        int n = cJSON_GetArraySize(symbols);
        if (!n)
            return fail(c, "symbols[] is empty");
        operand *ops = calloc((size_t)n, sizeof *ops);
        for (int i = 0; i < n; i++) {
            const char *s = cJSON_GetStringValue(cJSON_GetArrayItem(symbols, i));
            snprintf(ops[i].text, sizeof ops[i].text, "%s", s ? s : "");
        }
        int rc = read_operands(c, ops, n, "Symbols");
        free(ops);
        return rc;
    }
    if (!table || !*table)
        return fail(c, "pass symbols[] (fully qualified symbols) or deviceName + watchTableName");
    if (session_prepare(c, AF_PROJECT) != 0)
        return -1;
    nav_plc plc;
    sw_found t;
    if (sw_plc(c, &plc) != 0)
        return -1;
    if (sw_find(c, plc.software, SWC_WATCH_TABLES, table, &t) != 0)
        return -1;
    if (ensure_device_session(c, &plc) != 0)
        return -1;
    mxml_node_t *top = sw_export_tree(c, t.item);
    if (!top)
        return -1;
    mxml_node_t *ol = sml_child(sml_object(top), "ObjectList");
    int n = 0, cap = 0;
    operand *ops = NULL;
    for (mxml_node_t *e = sml_child(ol, NULL); e; e = sml_next(e, NULL)) {
        if (strstr(mxmlGetElement(e), "CommentEntry"))
            continue;
        mxml_node_t *al = sml_child(e, "AttributeList");
        const char *name = sml_child_text(al, "Name");
        if (!name || !*name)
            name = sml_child_text(al, "Address");
        if (!name || !*name)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            operand *o = realloc(ops, (size_t)cap * sizeof *o);
            if (!o)
                abort();
            ops = o;
        }
        memset(&ops[n], 0, sizeof ops[n]);
        snprintf(ops[n].text, sizeof ops[n].text, "%s", name);
        n++;
    }
    mxmlDelete(top);
    char title[600];
    snprintf(title, sizeof title, "Watch table %s%s%s of %s", t.folder, *t.folder ? "/" : "", t.name, plc.device_name);
    int rc = n ? read_operands(c, ops, n, title) : (out(c, "%s has no entries.\n", title), 0);
    free(ops);
    return rc;
}

/* ---- diagnostics ------------------------------------------------------------------------- */

static int language_id(tool_ctx *c)
{
    const char *lang = arg_s(c, "language");
    if (!lang || !*lang)
        return 1033;
    if (isdigit((unsigned char)*lang))
        return atoi(lang);
    wchar_t *w = utf8_to_wide(lang);
    LCID lcid = w && IsValidLocaleName(w) ? LocaleNameToLCID(w, 0) : 0;
    free(w);
    if (lcid == 0 || lcid == LOCALE_CUSTOM_UNSPECIFIED) {
        out(c, "(unknown language '%s': using en-US)\n", lang);
        return 1033;
    }
    return (int)lcid;
}

static int a_read_diagnostics(tool_ctx *c)
{
    if (live_require(c) != 0)
        return -1;
    const char *kind = arg_s(c, "kind");
    if (!kind || !*kind)
        kind = "all";
    int all = _stricmp(kind, "all") == 0;
    if (!all && _stricmp(kind, "identity") != 0 && _stricmp(kind, "protection") != 0 && _stricmp(kind, "alarms") != 0 &&
        _stricmp(kind, "buffer") != 0)
        return fail(c, "kind must be all, identity, protection, alarms or buffer");
    if (_stricmp(kind, "buffer") == 0)
        return fail(c, "the diagnostic buffer is not readable through the S7CommPlus driver used for live data: use TIA "
                       "Portal (Online & diagnostics > Diagnostic buffer). kind=alarms lists the active alarms.");
    int max = (int)arg_i(c, "maxEntries", 20);
    if (max < 1)
        max = 1;
    if (all || _stricmp(kind, "identity") == 0) {
        const live_ident *id = live_identity();
        out(c, "Identity (read online from %s):\n  Order number: %s\n  Firmware: %s\n", live_host(),
            id->order[0] ? id->order : "?", id->firmware[0] ? id->firmware : "?");
        if (id->simulated)
            out(c, "  Simulation: yes (PLCSIM reports its own identity, not the configured CPU)\n");
        out(c, "  Session identification: %s\n", id->raw);
    }
    if (all || _stricmp(kind, "protection") == 0) {
        int level = live_access_level(c);
        if (level < 0)
            return -1;
        out(c, "Protection: effective access level %d = %s for this session%s\n", level, live_access_name(level),
            live_used_credential() ? " (a stored PLC credential was offered: it is used when the PLC asks for a login)" : "");
    }
    if (all || _stricmp(kind, "alarms") == 0) {
        int lcid = language_id(c);
        if (live_print_active_alarms(c, lcid, max) != 0)
            return -1;
        live_print_configured_alarms(c, lcid, max);
    }
    if (all)
        out(c, "Diagnostic buffer: not readable through the S7CommPlus driver (TIA Portal: Online & diagnostics).\n");
    return 0;
}

static const action_def actions[] = {
    { "connect", "host (PLC IP) or deviceName; optional timeoutMs=5000, reconnect=false",
      "Open the live-data session (S7CommPlus over TLS, TCP port 102) to a PLC: S7-1500 from firmware V2.9, S7-1200 from "
      "V4.3, PLCSIM / PLCSIM Advanced. One session at a time; connecting to another PLC closes the current one. A "
      "protected PLC gets its password / PLC user from the Windows Credential Manager (action=set_credential): "
      "passwords are never tool arguments. Reports the CPU order number and firmware read online and the access level.",
      a_connect, AF_BRIDGE },
    { "delete_credential", "host", "Remove the stored PLC credential of a host (PLC IP, or * for the any-PLC entry).",
      a_delete_credential, 0 },
    { "disconnect", "", "Close the live-data session.", a_disconnect, AF_BRIDGE },
    { "get_status", "",
      "Live-data readiness (driver and OpenSSL files), session state, PLC identity and access level. Safe anytime.",
      a_get_status, 0 },
    { "list_credentials", "", "List stored PLC credentials (host, user). Never shows the secret.", a_list_credentials, 0 },
    { "list_readable", "optional root, limit=200, refresh=false",
      "What the PLC exposes for reading. Without root: one line per root (each DB, IArea/QArea/MArea for the PLC tags) "
      "with its value count; with root (a DB name, or I/Q/M) the values of that root with their data types. Use it when "
      "a DB or tag does not resolve: it separates 'not in the PLC / not accessible' from a wrong name. The browse is "
      "cached per session; refresh=true reads it again (after a download).",
      a_list_readable, AF_BRIDGE },
    { "read_db", "dbName; optional members[], limit=1000, refresh=false",
      "Live values of a data block (optimized or standard), symbolic, in TIA notation (16#.., T#.., DTL#..), with the "
      "data type of each value. members[] limits the read to members or structures (\"speed\", \"motor.state\").",
      a_read_db, AF_BRIDGE },
    { "read_diagnostics", "optional kind=all|identity|protection|alarms|buffer, maxEntries=20, language=en-US",
      "CPU order number and firmware read online (Openness cannot read them), effective protection level, active "
      "alarms (texts in language) and the alarms configured in the PLC. kind=buffer: the diagnostic buffer is not "
      "readable through the S7CommPlus driver.",
      a_read_diagnostics, AF_BRIDGE },
    { "read_tag_table", "deviceName, tagTableName; optional tags[], host",
      "Live values of a project tag table: the tags are read symbolically from the PLC. tags[] limits to tag names or "
      "their absolute addresses (%M1.0, %MW20, %I0.0...). Without a session it connects to the device's configured IP "
      "(or host); it refuses a session whose CPU order number differs from the project device.",
      a_read_tag_table, AF_BRIDGE | AF_PROJECT },
    { "read_watch_table", "EITHER symbols[] OR deviceName, watchTableName; optional host",
      "Live values of fully qualified symbols (symbols[]: \"DB\".member, DB.arr[2], \"Tag\") or of the entries of a "
      "project watch table. Passing both is an error. Absolute addresses without a tag, peripheral (:P) and slice "
      "operands are reported as not readable.",
      a_read_watch_table, AF_BRIDGE },
    { "set_credential", "host; optional global=false",
      "Ask the USER for the PLC password (user name \"-\") or PLC user in the standard Windows credential dialog on this "
      "PC and store it in the Windows Credential Manager (tiaComandante/plc/<host>; host * = any PLC). Used by connect "
      "and by go_online, download and upload. Never ask the user to type a password in the chat.",
      a_set_credential, 0 },
};

const tool_def tool_live_data = {
    .name = "live_data",
    .title = "Live PLC data",
    .summary = "Live PLC values and CPU information over S7CommPlus (TLS), read-only: data blocks, tag and watch tables, "
               "symbols, CPU identity, protection level, alarms. Needs no TIA Portal for reading DBs and symbols; tag and "
               "watch tables come from the open project. The PLC must allow secure PG/HMI communication (S7-1500 "
               "V2.9+, S7-1200 V4.3+, PLCSIM).",
    .properties =
        "{"
        "\"host\":{\"type\":\"string\",\"description\":\"PLC IP address (connect, credentials; read_tag_table / "
        "read_watch_table when no session is open).\"},"
        "\"deviceName\":{\"type\":\"string\",\"description\":\"Project device or PLC name.\"},"
        "\"timeoutMs\":{\"type\":\"integer\",\"description\":\"connect: answer timeout in ms (default 5000).\"},"
        "\"reconnect\":{\"type\":\"boolean\",\"description\":\"connect: open a new session even if one to host is open.\"},"
        "\"root\":{\"type\":\"string\",\"description\":\"list_readable: DB name, or I/Q/M (IArea/QArea/MArea).\"},"
        "\"limit\":{\"type\":\"integer\",\"description\":\"list_readable (default 200), read_db (default 1000): maximum "
        "values.\"},"
        "\"refresh\":{\"type\":\"boolean\",\"description\":\"list_readable, read_db: browse the PLC again.\"},"
        "\"dbName\":{\"type\":\"string\",\"description\":\"read_db: data block name.\"},"
        "\"members\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"read_db: members or "
        "structures to read.\"},"
        "\"tagTableName\":{\"type\":\"string\"},"
        "\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"read_tag_table: tag names or "
        "absolute addresses.\"},"
        "\"watchTableName\":{\"type\":\"string\"},"
        "\"symbols\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"read_watch_table: fully "
        "qualified symbols.\"},"
        "\"kind\":{\"type\":\"string\",\"enum\":[\"all\",\"identity\",\"protection\",\"alarms\",\"buffer\"]},"
        "\"maxEntries\":{\"type\":\"integer\",\"description\":\"read_diagnostics: maximum alarms listed (default 20).\"},"
        "\"language\":{\"type\":\"string\",\"description\":\"read_diagnostics: alarm text language, e.g. en-US, de-DE, "
        "it-IT or an LCID (default en-US).\"},"
        "\"global\":{\"type\":\"boolean\",\"description\":\"set_credential: global PLC user instead of a project user.\"}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
    .hints = 0,
};
