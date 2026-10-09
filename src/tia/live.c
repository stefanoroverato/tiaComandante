#include "live.h"

#include "app/config.h"
#include "app/credentials.h"
#include "tia/session.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/utf.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define T_CONNECTION "S7CommPlusDriver.S7CommPlusConnection"
#define T_ADDRESS "S7CommPlusDriver.ItemAddress"
#define T_TAGS "S7CommPlusDriver.ClientApi.PlcTags"
#define T_TAG "S7CommPlusDriver.ClientApi.PlcTag"

/* Files installed next to the server by the build (see CMakeLists.txt). */
static const char *const k_files[] = { "S7CommPlusDriver.dll", "zlib.net.dll", "libcrypto-3-x64.dll", "libssl-3-x64.dll" };

static struct {
    int loaded;
    th conn; /* pinned S7CommPlusConnection, 0 = no session */
    char host[64];
    live_ident id;
    long long since;
    int used_cred;
    live_var *vars;
    int nvars;
    double browse_ms;
} L;

/* ---- driver files ------------------------------------------------------------------------ */

const char *live_missing_file(void)
{
    char dir[TC_PATH_MAX], p[TC_PATH_MAX];
    if (fs_exe_dir(dir, sizeof dir) != 0)
        return k_files[0];
    for (int i = 0; i < (int)(sizeof k_files / sizeof k_files[0]); i++) {
        fs_join(p, sizeof p, dir, k_files[i]);
        if (!fs_is_file(p))
            return k_files[i];
    }
    return NULL;
}

static void file_version(const char *path, char *out, size_t cap)
{
    *out = 0;
    wchar_t *w = utf8_to_wide(path);
    DWORD dummy = 0, n = w ? GetFileVersionInfoSizeW(w, &dummy) : 0;
    void *buf = n ? malloc(n) : NULL;
    VS_FIXEDFILEINFO *fi = NULL;
    UINT len = 0;
    if (buf && GetFileVersionInfoW(w, 0, n, buf) && VerQueryValueW(buf, L"\\", (void **)&fi, &len) && fi)
        snprintf(out, cap, "%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
    free(buf);
    free(w);
}

void live_describe_driver(strbuf *sb)
{
    char dir[TC_PATH_MAX], p[TC_PATH_MAX], ver[64];
    if (fs_exe_dir(dir, sizeof dir) != 0)
        return;
    for (int i = 0; i < (int)(sizeof k_files / sizeof k_files[0]); i++) {
        fs_join(p, sizeof p, dir, k_files[i]);
        if (!fs_is_file(p)) {
            sb_printf(sb, "  %s: MISSING\n", k_files[i]);
            continue;
        }
        file_version(p, ver, sizeof ver);
        sb_printf(sb, "  %s %s\n", k_files[i], *ver ? ver : "(no version)");
    }
}

static int load_driver(tool_ctx *c)
{
    if (L.loaded)
        return 0;
    if (session_bridge(c) != 0)
        return -1;
    const char *missing = live_missing_file();
    if (missing)
        return fail(c, "live data is not installed: %s is missing next to the server (build tiaComandante with the "
                       "third_party/s7commplus submodule and a C# 7.3 compiler, see README)", missing);
    char dir[TC_PATH_MAX], p[TC_PATH_MAX];
    fs_exe_dir(dir, sizeof dir);
    /* OpenSSL by full path: the driver's DllImport then finds the loaded modules by name. */
    for (int i = 2; i < 4; i++) {
        fs_join(p, sizeof p, dir, k_files[i]);
        wchar_t *w = utf8_to_wide(p);
        HMODULE h = w ? LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) : NULL;
        free(w);
        if (!h)
            return fail(c, "cannot load %s (Windows error %lu)", p, GetLastError());
    }
    fs_join(p, sizeof p, dir, k_files[0]);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "op", "load");
    cJSON_AddStringToObject(r, "path", p);
    cJSON *a = td_request(r);
    if (!a)
        return fail_td(c, "cannot load S7CommPlusDriver.dll");
    cJSON_Delete(a);
    L.loaded = 1;
    LOG_I("S7CommPlusDriver loaded from %s", p);
    return 0;
}

/* ---- errors ------------------------------------------------------------------------------ */

static const struct {
    long long code;
    const char *text;
} k_errors[] = {
    { 0x1, "TCP socket creation failed" },
    { 0x2, "TCP connection timeout" },
    { 0x3, "TCP connection failed" },
    { 0x4, "TCP receive timeout" },
    { 0x5, "no answer from the PLC (timeout)" },
    { 0x6, "TCP send timeout" },
    { 0x7, "TCP send failed" },
    { 0x8, "connection reset by the PLC" },
    { 0x9, "not connected" },
    { 0x2751, "host unreachable" },
    { 0x10000, "the PLC refused the ISO connection" },
    { 0x30000, "unexpected answer from the PLC (protocol error)" },
    { 0x1E10000, "access denied: wrong password or PLC user" },
    { 0x2800000, "firmware not supported (S7CommPlus with TLS needs S7-1500 V2.9+, S7-1200 V4.3+, software "
                 "controllers V21.9+)" },
    { 0x2900000, "device type not supported by the driver" },
    { 0x3100000, "TLS error (OpenSSL)" },
};

static const char *err_text(long long code)
{
    for (size_t i = 0; i < sizeof k_errors / sizeof k_errors[0]; i++)
        if (k_errors[i].code == code)
            return k_errors[i].text;
    return "driver error";
}

/* Transport and protocol errors leave the session unusable. */
static int is_session_error(long long code)
{
    return (code >= 0x1 && code <= 0x9) || code == 0x2751 || code == 0x10000 || code == 0x30000;
}

static int driver_error(tool_ctx *c, const char *what, long long code)
{
    if (L.conn && is_session_error(code)) {
        char host[64];
        snprintf(host, sizeof host, "%s", L.host);
        live_disconnect();
        return fail(c, "%s: %s (code 16#%llX). The live-data session to %s was closed: connect again "
                       "(live_data action=connect).", what, err_text(code), (unsigned long long)code, host);
    }
    return fail(c, "%s: %s (code 16#%llX)", what, err_text(code), (unsigned long long)code);
}

/* ---- session ----------------------------------------------------------------------------- */

int live_connected(void) { return L.conn != 0; }
const char *live_host(void) { return L.host; }
const live_ident *live_identity(void) { return &L.id; }
long long live_since(void) { return L.since; }
int live_used_credential(void) { return L.used_cred; }

static void trim_copy(char *out, size_t cap, const char *s, size_t n)
{
    while (n && (*s == ' ' || *s == '\t'))
        s++, n--;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        n--;
    snprintf(out, cap, "%.*s", (int)n, s);
}

/* "1;6ES7 511-1AK02-0AB0 ;V2.9" -> order and firmware. */
static void parse_ident(live_ident *id, const char *raw)
{
    memset(id, 0, sizeof *id);
    snprintf(id->raw, sizeof id->raw, "%s", raw ? raw : "");
    const char *a = strchr(id->raw, ';');
    const char *b = a ? strchr(a + 1, ';') : NULL;
    if (!a || !b)
        return;
    trim_copy(id->order, sizeof id->order, a + 1, (size_t)(b - a - 1));
    trim_copy(id->firmware, sizeof id->firmware, b + 1, strlen(b + 1));
    id->simulated = strstr(id->order, "SIM") != NULL;
}

static void free_vars(void)
{
    for (int i = 0; i < L.nvars; i++) {
        free(L.vars[i].name);
        free(L.vars[i].access);
    }
    free(L.vars);
    L.vars = NULL;
    L.nvars = 0;
}

void live_disconnect(void)
{
    if (L.conn) {
        LOG_I("closing the live-data session to %s", L.host);
        td_call_v(L.conn, "Disconnect", NULL);
        td_clear_err();
        td_release(L.conn);
    }
    free_vars();
    L.conn = 0;
    L.host[0] = 0;
    memset(&L.id, 0, sizeof L.id);
    L.since = 0;
    L.used_cred = 0;
}

const char *live_access_name(int level)
{
    switch (level) {
    case 1: return "full access";
    case 2: return "read access";
    case 3: return "HMI access";
    case 4: return "no access";
    default: return "unknown";
    }
}

int live_access_level(tool_ctx *c)
{
    if (!L.conn)
        return -1;
    cJSON *v = td_call_outs(L.conn, "GetEffectiveProtectionLevel", NULL);
    if (!v) {
        fail_td(c, "reading the protection level failed");
        return -1;
    }
    long long res = tdv_i(cJSON_GetObjectItemCaseSensitive(v, "ret"), -1);
    int level = (int)tdv_i(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(v, "out"), 0), 0);
    cJSON_Delete(v);
    if (res != 0) {
        driver_error(c, "reading the protection level failed", res);
        return -1;
    }
    return level;
}

int live_require(tool_ctx *c)
{
    if (L.conn)
        return 0;
    return fail(c, "no live-data session: open one with live_data action=connect host=<PLC IP>");
}

int live_connect(tool_ctx *c, const char *host, int timeout_ms)
{
    if (load_driver(c) != 0)
        return -1;
    if (L.conn) {
        out(c, "Closing the live-data session to %s.\n", L.host);
        live_disconnect();
    }
    th conn = td_new(T_CONNECTION, NULL);
    if (!conn)
        return fail_td(c, "cannot create the S7CommPlus connection");
    cred cr;
    memset(&cr, 0, sizeof cr);
    int have = cred_get(CRED_PLC, host, &cr) == 0 && cr.password;
    /* user "-" (or none) = access-level password only */
    const char *user = have && cr.user[0] && strcmp(cr.user, "-") != 0 ? cr.user : "";
    DWORD t0 = GetTickCount();
    cJSON *v = td_call(conn, "Connect", tda("sssI", host, have ? cr.password : "", user, timeout_ms));
    DWORD ms = GetTickCount() - t0;
    if (have)
        cred_free(&cr);
    if (!v) {
        td_release(conn);
        return fail_td(c, "S7CommPlus connection failed");
    }
    long long res = tdv_i(v, -1);
    cJSON_Delete(v);
    if (res != 0) {
        td_release(conn);
        fail(c, "connection to %s failed: %s (code 16#%llX)", host, err_text(res), (unsigned long long)res);
        if (res == 0x1E10000)
            out(c, "The stored PLC credential was refused: store the right one with live_data action=set_credential "
                   "host=%s (Windows dialog).\n", host);
        else if (res == 0x3100000 || res == 0x5 || res == 0x30000)
            out(c, "The CPU must allow secure PG/HMI communication (TLS): S7-1500 from firmware V2.9, S7-1200 from V4.3, "
                   "PLCSIM / PLCSIM Advanced from V4.0, configured with TIA Portal V17 or later. The driver does not "
                   "support legacy communication.\n");
        else if (res <= 0x9 || res == 0x2751)
            out(c, "Check the IP address and that TCP port 102 of the PLC is reachable from this PC.\n");
        return -1;
    }
    td_pin(conn);
    L.conn = conn;
    snprintf(L.host, sizeof L.host, "%s", host);
    L.since = (long long)time(NULL);
    L.used_cred = have;
    char *raw = td_get_s(conn, "SessionVersionPAOMString");
    td_clear_err();
    parse_ident(&L.id, raw);
    free(raw);
    LOG_I("live-data session to %s (%s) in %lu ms", host, L.id.raw, (unsigned long)ms);

    out(c, "Connected to %s in %lu ms (S7CommPlus over TLS).\n", host, (unsigned long)ms);
    if (L.id.order[0])
        out(c, "PLC: %s, firmware %s%s\n", L.id.order, L.id.firmware[0] ? L.id.firmware : "?",
            L.id.simulated ? " (simulation: PLCSIM reports itself, not the configured CPU)" : "");
    int level = live_access_level(c);
    if (level < 0)
        return -1;
    out(c, "Access level: %s%s\n", live_access_name(level),
        have ? " (a stored PLC credential was offered: it is used when the PLC asks for a login)" : "");
    if (level >= 4)
        out(c, "Warning: the PLC is fully protected and no access was granted: values are not readable. Store the PLC "
               "password with live_data action=set_credential host=%s (Windows dialog), then connect again.\n", host);
    return 0;
}

/* ---- alarms ------------------------------------------------------------------------------ */

static int parse_iso(const char *s, int f[6], long long *ticks7);

static const char *alarm_domain(long long d)
{
    if (d == 1)
        return "system diagnostics";
    if (d == 2)
        return "security";
    if (d >= 256 && d <= 272)
        return "user alarm class";
    return "other";
}

/* UTC timestamp "2026-10-09T18:00:00.1234567Z" -> "2026-10-09 18:00:00.123 UTC"; unset (1970) -> "". */
static void fmt_utc(char *out, size_t cap, const char *iso)
{
    int f[6];
    long long t7;
    *out = 0;
    if (parse_iso(iso, f, &t7) != 0 || f[0] <= 1970)
        return;
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03lld UTC", f[0], f[1], f[2], f[3], f[4], f[5], t7 / 10000);
}

static void alarm_id(char *out, size_t cap, const cJSON *v)
{
    long long i;
    unsigned long long u;
    int uns;
    if (tdv_int64(v, &i, &u, &uns) == 0)
        snprintf(out, cap, "16#%llX", u);
    else
        snprintf(out, cap, "?");
}

/* Alarm texts can span lines and hold associated-value fields (@1W%t#7W@): kept on one line, fields kept. */
static void flatten(strbuf *sb, const char *s)
{
    size_t start = sb->len;
    for (; s && *s; s++) {
        if (*s == '\r')
            continue;
        if (*s == '\n') {
            while (sb->len > start && sb->p[sb->len - 1] == ' ')
                sb->p[--sb->len] = 0;
            sb_append(sb, " / ");
            while (s[1] == ' ')
                s++;
        } else {
            sb_appendn(sb, s, 1);
        }
    }
    size_t i = start;
    while (i < sb->len && (sb->p[i] == ' ' || sb->p[i] == '/'))
        i++;
    if (i == sb->len) {
        if (sb->len > start) {
            sb->len = start;
            sb->p[start] = 0;
        }
        sb_append(sb, "(no text)");
    }
}

static void out_text(tool_ctx *c, const char *s)
{
    strbuf sb;
    sb_init(&sb);
    flatten(&sb, s);
    out_raw(c, sb_str(&sb));
    sb_free(&sb);
}

int live_print_active_alarms(tool_ctx *c, int lcid, int max)
{
    if (live_require(c) != 0)
        return -1;
    cJSON *v = td_call_outs(L.conn, "GetActiveAlarms", tda("nI", lcid));
    if (!v)
        return fail_td(c, "reading the active alarms failed");
    long long res = tdv_i(cJSON_GetObjectItemCaseSensitive(v, "ret"), -1);
    th list = tdv_h(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(v, "out"), 0));
    cJSON_Delete(v);
    if (res != 0)
        return driver_error(c, "reading the active alarms failed", res);
    cJSON *items = list ? td_enum(list, "CpuAlarmId,AlarmDomain,MessageType,AlarmTexts,AsCgs", -1) : NULL;
    if (!items)
        return fail_td(c, "reading the active alarms failed");
    int n = cJSON_GetArraySize(items), k = 0;
    out(c, "Active alarms: %d\n", n);
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        if (k++ >= max) {
            out(c, "  ... %d more (raise maxEntries)\n", n - max);
            break;
        }
        th texts = tdv_h(tdi_a(it, "AlarmTexts")), cgs = tdv_h(tdi_a(it, "AsCgs"));
        cJSON *t = texts ? td_attrs(texts, "AlarmText,Infotext") : NULL;
        cJSON *a = cgs ? td_attrs(cgs, "SubtypeId,Timestamp,AckTimestamp") : NULL;
        td_clear_err();
        char id[32], since[64], acked[64];
        alarm_id(id, sizeof id, tdi_a(it, "CpuAlarmId"));
        fmt_utc(since, sizeof since, tdv_s(cJSON_GetObjectItemCaseSensitive(a, "Timestamp")));
        fmt_utc(acked, sizeof acked, tdv_s(cJSON_GetObjectItemCaseSensitive(a, "AckTimestamp")));
        long long sub = tdv_i(cJSON_GetObjectItemCaseSensitive(a, "SubtypeId"), 0);
        const char *text = tdv_s(cJSON_GetObjectItemCaseSensitive(t, "AlarmText"));
        const char *info = tdv_s(cJSON_GetObjectItemCaseSensitive(t, "Infotext"));
        out(c, "  [%s] ", sub == 2677 ? "going" : "coming");
        out_text(c, text);
        out(c, "  (alarm %s, %s%s%s%s%s)\n", id, alarm_domain(tdi_i(it, "AlarmDomain", 0)), *since ? ", " : "", since,
            *acked ? ", acknowledged " : "", acked);
        if (info && *info) {
            out(c, "      info: ");
            out_text(c, info);
            out(c, "\n");
        }
        cJSON_Delete(t);
        cJSON_Delete(a);
    }
    cJSON_Delete(items);
    return 0;
}

static long long explore_alarms(int lcid, th *dict)
{
    *dict = td_new_generic("System.Collections.Generic.Dictionary`2", "System.UInt64,S7CommPlusDriver.AlarmData", NULL);
    cJSON *v = *dict ? td_call_outs(L.conn, "ExploreASAlarms", tda("hI", *dict, lcid)) : NULL;
    long long res = v ? tdv_i(cJSON_GetObjectItemCaseSensitive(v, "ret"), -1) : -1;
    cJSON_Delete(v);
    return res;
}

void live_print_configured_alarms(tool_ctx *c, int lcid, int max)
{
    if (!L.conn)
        return;
    th dict = 0;
    long long res = explore_alarms(lcid, &dict);
    if (res != 0 && !td_failed() && lcid != 1033) {
        /* The PLC answers with an error for a language it holds no alarm texts in. */
        out(c, "(configured alarms: no answer for language %d, texts in en-US)\n", lcid);
        res = explore_alarms(1033, &dict);
    }
    if (res != 0) {
        if (td_failed())
            out(c, "Configured alarms: not available (%s)\n", td_err());
        else
            out(c, "Configured alarms: not available (%s, code 16#%llX)\n", err_text(res), (unsigned long long)res);
        td_clear_err();
        return;
    }
    /* System diagnostics and security alarms are firmware templates: only counted. Program alarms are listed. */
    cJSON *items = td_enum(dict, "Key,Value", -1);
    int counts[3] = { 0, 0, 0 }, listed = 0, more = 0;
    strbuf list;
    sb_init(&list);
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        th data = tdv_h(tdi_a(it, "Value"));
        th stai = data ? td_get_h(data, "MultipleStai") : 0;
        cJSON *s = stai ? td_attrs(stai, "AlarmDomain,MessageType,AlarmEnabled") : NULL;
        td_clear_err();
        long long dom = tdv_i(cJSON_GetObjectItemCaseSensitive(s, "AlarmDomain"), 0);
        int bucket = dom >= 256 && dom <= 272 ? 2 : dom == 2 ? 1 : 0;
        counts[bucket]++;
        if (bucket == 2) {
            if (listed >= max) {
                more++;
            } else {
                static const char *const types[] = { "?", "alarm", "notify", "info report", "event ack" };
                long long mt = tdv_i(cJSON_GetObjectItemCaseSensitive(s, "MessageType"), 0);
                th al = td_get_h(data, "AlText");
                char *text = al ? td_get_s(al, "AlarmText") : NULL;
                td_clear_err();
                char id[32];
                alarm_id(id, sizeof id, tdi_a(it, "Key"));
                sb_printf(&list, "  %s  ", id);
                flatten(&list, text);
                sb_printf(&list, " (%s, class %lld%s)\n", types[mt >= 1 && mt <= 4 ? mt : 0], dom - 256,
                          tdv_i(cJSON_GetObjectItemCaseSensitive(s, "AlarmEnabled"), 1) ? "" : ", disabled");
                free(text);
                listed++;
            }
        }
        cJSON_Delete(s);
    }
    cJSON_Delete(items);
    td_clear_err();
    out(c, "Configured alarms in the PLC: %d (system diagnostics and other: %d, security: %d, program alarms: %d)\n",
        counts[0] + counts[1] + counts[2], counts[0], counts[1], counts[2]);
    out_raw(c, sb_str(&list));
    if (more)
        out(c, "  ... %d more program alarm(s) (raise maxEntries)\n", more);
    sb_free(&list);
}

/* ---- browse ------------------------------------------------------------------------------ */

static const struct {
    unsigned sdt;
    const char *name;
} k_types[] = {
    { 1, "Bool" },          { 2, "Byte" },          { 3, "Char" },          { 4, "Word" },          { 5, "Int" },
    { 6, "DWord" },         { 7, "DInt" },          { 8, "Real" },          { 9, "Date" },          { 10, "Time_Of_Day" },
    { 11, "Time" },         { 12, "S5Time" },       { 14, "Date_And_Time" }, { 19, "String" },       { 20, "Pointer" },
    { 22, "Any" },          { 23, "Block_FB" },     { 24, "Block_FC" },     { 28, "Counter" },      { 29, "Timer" },
    { 40, "Bool" },         { 48, "LReal" },        { 49, "ULInt" },        { 50, "LInt" },         { 51, "LWord" },
    { 52, "USInt" },        { 53, "UInt" },         { 54, "UDInt" },        { 55, "SInt" },         { 61, "WChar" },
    { 62, "WString" },      { 63, "Variant" },      { 64, "LTime" },        { 65, "LTime_Of_Day" }, { 66, "LDT" },
    { 67, "DTL" },          { 96, "Remote" },       { 128, "AOM_IDENT" },   { 129, "EVENT_ANY" },   { 130, "EVENT_ATT" },
    { 132, "AOM_AID" },     { 133, "AOM_LINK" },    { 134, "EVENT_HWINT" }, { 144, "HW_ANY" },      { 145, "HW_IOSYSTEM" },
    { 146, "HW_DPMASTER" }, { 147, "HW_DEVICE" },   { 148, "HW_DPSLAVE" },  { 149, "HW_IO" },       { 150, "HW_MODULE" },
    { 151, "HW_SUBMODULE" }, { 152, "HW_HSC" },     { 153, "HW_PWM" },      { 154, "HW_PTO" },      { 155, "HW_INTERFACE" },
    { 156, "HW_IEPORT" },   { 160, "OB_ANY" },      { 161, "OB_DELAY" },    { 162, "OB_TOD" },      { 163, "OB_CYCLIC" },
    { 164, "OB_ATT" },      { 168, "CONN_ANY" },    { 169, "CONN_PRG" },    { 170, "CONN_OUC" },    { 171, "CONN_R_ID" },
    { 173, "PORT" },        { 174, "RTM" },         { 175, "PIP" },         { 192, "OB_PCYCLE" },   { 193, "OB_HWINT" },
    { 195, "OB_DIAG" },     { 196, "OB_TIMEERROR" }, { 197, "OB_STARTUP" }, { 208, "DB_ANY" },      { 209, "DB_WWW" },
    { 210, "DB_DYN" },
};

const char *live_type_name(unsigned sdt)
{
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++)
        if (k_types[i].sdt == sdt)
            return k_types[i].name;
    static __declspec(thread) char buf[32];
    snprintf(buf, sizeof buf, "type %u", sdt);
    return buf;
}

int live_browse(tool_ctx *c, int refresh, const live_var **vars, int *n, double *ms)
{
    if (live_require(c) != 0)
        return -1;
    if (L.vars && !refresh)
        goto done;
    free_vars();
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    cJSON *v = td_call_outs(L.conn, "Browse", NULL);
    if (!v)
        return fail_td(c, "browsing the PLC failed");
    long long res = tdv_i(cJSON_GetObjectItemCaseSensitive(v, "ret"), -1);
    th list = tdv_h(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(v, "out"), 0));
    cJSON_Delete(v);
    if (res != 0)
        return driver_error(c, "browsing the PLC failed", res);
    cJSON *items = list ? td_enum(list, "Name,AccessSequence,Softdatatype", -1) : NULL;
    if (!items)
        return fail_td(c, "reading the browse result failed");
    int cnt = cJSON_GetArraySize(items);
    L.vars = calloc(cnt ? (size_t)cnt : 1, sizeof *L.vars);
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        const char *name = tdi_s(it, "Name"), *acc = tdi_s(it, "AccessSequence");
        if (!name || !acc)
            continue;
        live_var *lv = &L.vars[L.nvars++];
        lv->name = tc_strdup(name);
        lv->access = tc_strdup(acc);
        lv->sdt = (unsigned)tdi_i(it, "Softdatatype", 0);
    }
    cJSON_Delete(items);
    QueryPerformanceCounter(&t1);
    L.browse_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    LOG_I("browsed %d variables on %s in %.0f ms", L.nvars, L.host, L.browse_ms);
done:
    *vars = L.vars;
    *n = L.nvars;
    if (ms)
        *ms = L.browse_ms;
    return 0;
}

/* ---- value formatting (TIA notation) ------------------------------------------------------ */

enum {
    SDT_BOOL = 1, SDT_BYTE = 2, SDT_CHAR = 3, SDT_WORD = 4, SDT_DWORD = 6, SDT_REAL = 8, SDT_DATE = 9, SDT_TOD = 10,
    SDT_TIME = 11, SDT_S5TIME = 12, SDT_DT = 14, SDT_STRING = 19, SDT_POINTER = 20, SDT_ANY = 22, SDT_BBOOL = 40,
    SDT_LREAL = 48, SDT_LWORD = 51, SDT_WCHAR = 61, SDT_WSTRING = 62, SDT_LTIME = 64, SDT_LTOD = 65, SDT_LDT = 66,
    SDT_DTL = 67, SDT_REMOTE = 96,
};

/* T#1D_2H_3M_4S_5MS / LT#..._6US_7NS: parts that are zero are left out. */
static void fmt_duration(strbuf *sb, const char *prefix, long long v, int nanos)
{
    static const unsigned long long ms_units[] = { 86400000ULL, 3600000ULL, 60000ULL, 1000ULL, 1ULL };
    static const unsigned long long ns_units[] = { 86400000000000ULL, 3600000000000ULL, 60000000000ULL, 1000000000ULL,
                                                   1000000ULL, 1000ULL, 1ULL };
    static const char *const names[] = { "D", "H", "M", "S", "MS", "US", "NS" };
    const unsigned long long *units = nanos ? ns_units : ms_units;
    int nunits = nanos ? 7 : 5;
    unsigned long long mag = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    sb_printf(sb, "%s%s", prefix, v < 0 ? "-" : "");
    if (mag == 0) {
        sb_printf(sb, "0%s", names[nunits - 1]);
        return;
    }
    int first = 1;
    for (int i = 0; i < nunits; i++) {
        unsigned long long part = mag / units[i];
        mag %= units[i];
        if (!part)
            continue;
        sb_printf(sb, "%s%llu%s", first ? "" : "_", part, names[i]);
        first = 0;
    }
}

/* .NET DateTime in round-trip format "2026-10-09T18:00:00.1234567[Z|+01:00]". */
static int parse_iso(const char *s, int f[6], long long *ticks7)
{
    *ticks7 = 0;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5]) != 6)
        return -1;
    const char *dot = strchr(s, '.');
    if (dot) {
        int digits = 0;
        for (const char *p = dot + 1; *p >= '0' && *p <= '9' && digits < 7; p++, digits++)
            *ticks7 = *ticks7 * 10 + (*p - '0');
        for (; digits < 7; digits++)
            *ticks7 *= 10;
    }
    return 0;
}

static void fmt_quoted(strbuf *sb, const char *s)
{
    sb_append(sb, "'");
    for (; *s; s++) {
        if (*s == '\'')
            sb_append(sb, "$'");
        else if (*s == '$')
            sb_append(sb, "$$");
        else
            sb_appendn(sb, s, 1);
    }
    sb_append(sb, "'");
}

/* Shortest text that reads back as the same Real (single) or LReal value. */
static void fmt_real(strbuf *sb, double d, int single)
{
    char buf[64];
    for (int prec = single ? 6 : 15; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*g", prec, d);
        double back = strtod(buf, NULL);
        if (single ? (float)back == (float)d : back == d)
            break;
    }
    sb_append(sb, buf);
}

static void fmt_scalar(strbuf *sb, const cJSON *v, unsigned sdt, long long dtl_ns)
{
    long long i = 0;
    unsigned long long u = 0;
    int uns = 0, exact = tdv_int64(v, &i, &u, &uns) == 0;
    if (cJSON_IsBool(v)) {
        sb_append(sb, cJSON_IsTrue(v) ? "TRUE" : "FALSE");
        return;
    }
    if (cJSON_IsString(v)) {
        const char *s = v->valuestring;
        int f[6];
        long long t7;
        switch (sdt) {
        case SDT_CHAR: case SDT_WCHAR: case SDT_STRING: case SDT_WSTRING:
            fmt_quoted(sb, s);
            return;
        case SDT_DATE:
            if (parse_iso(s, f, &t7) == 0) {
                sb_printf(sb, "D#%04d-%02d-%02d", f[0], f[1], f[2]);
                return;
            }
            break;
        case SDT_DT:
            if (parse_iso(s, f, &t7) == 0) {
                sb_printf(sb, "DT#%04d-%02d-%02d-%02d:%02d:%02d.%03lld", f[0], f[1], f[2], f[3], f[4], f[5], t7 / 10000);
                return;
            }
            break;
        case SDT_DTL:
            if (parse_iso(s, f, &t7) == 0) {
                sb_printf(sb, "DTL#%04d-%02d-%02d-%02d:%02d:%02d.%09lld", f[0], f[1], f[2], f[3], f[4], f[5],
                          dtl_ns >= 0 ? dtl_ns : t7 * 100);
                return;
            }
            break;
        }
        sb_append(sb, s);
        return;
    }
    if (!exact && cJSON_IsNumber(v)) {
        sb_printf(sb, "%.15g", v->valuedouble);
        return;
    }
    if (!exact) {
        char *t = tdv_text(v);
        sb_append(sb, t ? t : "?");
        free(t);
        return;
    }
    switch (sdt) {
    case SDT_REAL:
        fmt_real(sb, v->valuedouble, 1);
        return;
    case SDT_LREAL:
        fmt_real(sb, v->valuedouble, 0);
        return;
    case SDT_BYTE: case SDT_POINTER: case SDT_ANY: case SDT_REMOTE:
        sb_printf(sb, "16#%02llX", u & 0xFF);
        return;
    case SDT_WORD:
        sb_printf(sb, "16#%04llX", u & 0xFFFF);
        return;
    case SDT_DWORD:
        sb_printf(sb, "16#%08llX", u & 0xFFFFFFFF);
        return;
    case SDT_LWORD:
        sb_printf(sb, "16#%016llX", u);
        return;
    case SDT_TIME:
        fmt_duration(sb, "T#", i, 0);
        return;
    case SDT_LTIME:
        fmt_duration(sb, "LT#", i, 1);
        return;
    case SDT_TOD:
        sb_printf(sb, "TOD#%02llu:%02llu:%02llu.%03llu", u / 3600000, u / 60000 % 60, u / 1000 % 60, u % 1000);
        return;
    case SDT_LTOD:
        sb_printf(sb, "LTOD#%02llu:%02llu:%02llu.%09llu", u / 3600000000000ULL, u / 60000000000ULL % 60,
                  u / 1000000000ULL % 60, u % 1000000000ULL);
        return;
    case SDT_LDT: {
        __time64_t secs = (__time64_t)(u / 1000000000ULL);
        struct tm tm;
        if (_gmtime64_s(&tm, &secs) == 0) {
            sb_printf(sb, "LDT#%04d-%02d-%02d-%02d:%02d:%02d.%09llu", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec, u % 1000000000ULL);
            return;
        }
        break;
    }
    }
    if (uns)
        sb_printf(sb, "%llu", u);
    else
        sb_printf(sb, "%lld", i);
}

/* S5Time from the tag's TimeValue (0..999) and TimeBase (10 ms, 100 ms, 1 s, 10 s). */
static void fmt_s5time(strbuf *sb, th tag)
{
    static const long long base_ms[] = { 10, 100, 1000, 10000 };
    long long value = 0, base = 0;
    if (td_get_i(tag, "TimeValue", &value) != 0 || td_get_i(tag, "TimeBase", &base) != 0 || base < 0 || base > 3) {
        td_clear_err();
        sb_append(sb, "?");
        return;
    }
    fmt_duration(sb, "S5T#", value * base_ms[base], 0);
}

static char *format_value(const cJSON *v, unsigned sdt, th tag)
{
    strbuf sb;
    sb_init(&sb);
    if (sdt == SDT_S5TIME) {
        fmt_s5time(&sb, tag);
    } else if (tdv_h(v)) {
        /* arrays (whole-array symbols, pointers): elements in one list */
        cJSON *el = td_enum(tdv_h(v), NULL, 200);
        int n = 0;
        const cJSON *e;
        sb_append(&sb, "[");
        cJSON_ArrayForEach(e, el)
        {
            if (n++)
                sb_append(&sb, ", ");
            fmt_scalar(&sb, e, sdt == SDT_BBOOL ? SDT_BOOL : sdt, -1);
        }
        sb_append(&sb, n >= 200 ? ", ...]" : "]");
        cJSON_Delete(el);
        td_clear_err();
    } else {
        long long ns = -1;
        if (sdt == SDT_DTL && td_get_i(tag, "ValueNanosecond", &ns) != 0) {
            ns = -1;
            td_clear_err();
        }
        fmt_scalar(&sb, v, sdt, ns);
    }
    return sb_detach(&sb);
}

/* ---- reading ----------------------------------------------------------------------------- */

void live_read_init(live_read *r)
{
    memset(r, 0, sizeof *r);
}

static int add_slot(live_read *r)
{
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 64;
        live_value *v = realloc(r->vals, (size_t)cap * sizeof *v);
        int *s = realloc(r->slot, (size_t)cap * sizeof *s);
        if (!v || !s)
            abort();
        r->vals = v;
        r->slot = s;
        r->cap = cap;
    }
    memset(&r->vals[r->n], 0, sizeof r->vals[r->n]);
    return r->n++;
}

static void add_tag(live_read *r, int idx, th tag)
{
    if (!r->list) {
        r->list = td_new_generic("System.Collections.Generic.List`1", T_TAG, NULL);
        if (!r->list) {
            snprintf(r->vals[idx].note, sizeof r->vals[idx].note, "internal error: %s", td_err());
            td_clear_err();
            return;
        }
    }
    if (td_call_v(r->list, "Add", tda("h", tag)) != 0) {
        snprintf(r->vals[idx].note, sizeof r->vals[idx].note, "internal error: %s", td_err());
        td_clear_err();
        return;
    }
    r->slot[r->nslots++] = idx;
}

int live_read_add_var(live_read *r, const live_var *v)
{
    int idx = add_slot(r);
    r->vals[idx].sdt = v->sdt;
    th adr = td_new(T_ADDRESS, tda("s", v->access));
    cJSON *t = adr ? td_static(T_TAGS, "TagFactory", tda("shi", v->name, adr, (long long)v->sdt)) : NULL;
    th tag = tdv_h(t);
    cJSON_Delete(t);
    if (!tag) {
        snprintf(r->vals[idx].note, sizeof r->vals[idx].note, "%s",
                 td_failed() ? td_err() : "data type not supported by the S7CommPlus driver");
        td_clear_err();
        return idx;
    }
    add_tag(r, idx, tag);
    return idx;
}

int live_read_add_symbol(live_read *r, const char *symbol)
{
    int idx = add_slot(r);
    cJSON *t = L.conn ? td_call(L.conn, "getPlcTagBySymbol", tda("s", symbol)) : NULL;
    th tag = tdv_h(t);
    cJSON_Delete(t);
    if (!tag) {
        if (td_failed())
            snprintf(r->vals[idx].note, sizeof r->vals[idx].note, "not resolved: %s", td_err());
        else
            snprintf(r->vals[idx].note, sizeof r->vals[idx].note,
                     "not found in the PLC, or not a single value (structure, DB, or not accessible from HMI)");
        td_clear_err();
        return idx;
    }
    long long sdt = 0;
    if (td_get_i(tag, "Datatype", &sdt) == 0)
        r->vals[idx].sdt = (unsigned)sdt;
    td_clear_err();
    add_tag(r, idx, tag);
    return idx;
}

static unsigned long long u64_of(const cJSON *v)
{
    long long i;
    unsigned long long u;
    int uns;
    return tdv_int64(v, &i, &u, &uns) == 0 ? u : 0;
}

int live_read_exec(tool_ctx *c, live_read *r)
{
    if (live_require(c) != 0)
        return -1;
    if (r->nslots == 0)
        return 0;
    cJSON *v = td_static(T_TAGS, "ReadTags", tda("hh", L.conn, r->list));
    if (!v)
        return fail_td(c, "reading from the PLC failed");
    long long res = tdv_i(v, -1);
    cJSON_Delete(v);
    if (res != 0)
        return driver_error(c, "reading from the PLC failed", res);
    cJSON *items = td_enum(r->list, "Value,Quality,LastReadError", -1);
    if (!items)
        return fail_td(c, "reading the values failed");
    int k = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        if (k >= r->nslots)
            break;
        live_value *lv = &r->vals[r->slot[k++]];
        int q = (int)tdi_i(it, "Quality", 0);
        lv->good = (q & 0xC0) == 0xC0;
        if (lv->good) {
            lv->text = format_value(tdi_a(it, "Value"), lv->sdt, tdv_h(it));
        } else {
            unsigned long long e = u64_of(tdi_a(it, "LastReadError"));
            snprintf(lv->note, sizeof lv->note, "no value from the PLC (quality 16#%02X, error 16#%llX)", q & 0xFF, e);
        }
    }
    cJSON_Delete(items);
    return 0;
}

void live_read_free(live_read *r)
{
    for (int i = 0; i < r->n; i++)
        free(r->vals[i].text);
    free(r->vals);
    free(r->slot);
    memset(r, 0, sizeof *r);
}
