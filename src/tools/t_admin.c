/* admin: export store, statistics, system information. */
#include "tools.h"

#include "app/config.h"
#include "app/credentials.h"
#include "app/devcatalog.h"
#include "app/export_store.h"
#include "app/stats.h"
#include "mcp/registry.h"
#include "tc_version.h"
#include "tia/session.h"
#include "tia/tia_env.h"
#include "util/fs.h"
#include "util/utf.h"

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_text_type(const char *ct)
{
    return strncmp(ct, "text/", 5) == 0 || strstr(ct, "xml") || strstr(ct, "json");
}

static void export_line(tool_ctx *c, const export_info *e)
{
    char when[32], sz[32];
    fmt_unix_time(e->created, when, sizeof when);
    fmt_size(e->size, sz, sizeof sz);
    out(c, "%s  [name=%s, tool=%s, action=%s, type=%s, size=%s, created=%s]\n", e->id, e->name, e->tool, e->action,
        e->content_type, sz, when);
}

static int a_list_exports(tool_ctx *c)
{
    export_info *list = NULL;
    int n = 0;
    export_list(arg_s(c, "tool"), (int)arg_i(c, "limit", 20), &list, &n);
    out(c, "%d export(s) (kept 24 h):\n", n);
    for (int i = 0; i < n; i++)
        export_line(c, &list[i]);
    free(list);
    return 0;
}

static int a_get_export(tool_ctx *c)
{
    const char *id = arg_req(c, "exportId");
    if (!id)
        return -1;
    export_info e;
    if (export_get(id, &e) != 0)
        return fail(c, "export '%s' not found (expired after 24 h?)", id);
    if (!is_text_type(e.content_type))
        return fail(c, "export '%s' is binary (%s): use admin action=save_export to write it to a file", id, e.content_type);
    char *data = NULL;
    size_t len = 0;
    if (fs_read_all(e.path, &data, &len) != 0)
        return fail(c, "cannot read export data");
    long long offset = arg_i(c, "offset", 0);
    long long length = arg_i(c, "length", 60000);
    if (offset < 0)
        offset = 0;
    if ((size_t)offset > len)
        offset = (long long)len;
    if (length <= 0 || (size_t)(offset + length) > len)
        length = (long long)len - offset;
    /* Do not cut a UTF-8 sequence in half. */
    while (offset > 0 && ((unsigned char)data[offset] & 0xC0) == 0x80)
        offset--;
    size_t end = (size_t)(offset + length);
    while (end < len && ((unsigned char)data[end] & 0xC0) == 0x80)
        end++;
    if (!arg_b(c, "raw", 0))
        out(c, "exportId=%s name=%s type=%s bytes %lld-%zu of %zu%s\n---\n", e.id, e.name, e.content_type, offset, end, len,
            end < len ? " (more: call again with a larger offset)" : "");
    sb_appendn(&c->out, data + offset, end - (size_t)offset);
    free(data);
    return 0;
}

static int a_save_export(tool_ctx *c)
{
    const char *id = arg_req(c, "exportId");
    const char *dst = id ? arg_req(c, "outputPath") : NULL;
    if (!dst)
        return -1;
    export_info e;
    if (export_get(id, &e) != 0)
        return fail(c, "export '%s' not found (expired after 24 h?)", id);
    char full[TC_PATH_MAX], target[TC_PATH_MAX];
    if (fs_full_path(dst, full, sizeof full) != 0)
        return fail(c, "invalid outputPath '%s'", dst);
    if (fs_is_dir(full))
        fs_join(target, sizeof target, full, e.name);
    else
        snprintf(target, sizeof target, "%s", full);
    char parent[TC_PATH_MAX];
    snprintf(parent, sizeof parent, "%s", target);
    char *slash = strrchr(parent, '\\');
    if (slash) {
        *slash = 0;
        fs_mkdirs(parent);
    }
    if (fs_copy(e.path, target, 1) != 0)
        return fail(c, "cannot write '%s'", target);
    out(c, "Saved %s to %s. Ask the user whether to open it (admin action=open_file).\n", e.id, target);
    return 0;
}

static int a_delete_export(tool_ctx *c)
{
    const char *id = arg_req(c, "exportId");
    if (!id)
        return -1;
    if (export_delete(id) != 0)
        return fail(c, "export '%s' not found", id);
    out(c, "Deleted %s.\n", id);
    return 0;
}

static int a_clear_exports(tool_ctx *c)
{
    double hours = arg_d(c, "olderThanHours", 24);
    int n = export_clear(hours);
    out(c, "Deleted %d export(s) older than %.1f h.\n", n, hours);
    return 0;
}

static int a_open_file(tool_ctx *c)
{
    const char *p = arg_req(c, "filePath");
    if (!p)
        return -1;
    char full[TC_PATH_MAX];
    if (fs_full_path(p, full, sizeof full) != 0 || (!fs_is_file(full) && !fs_is_dir(full)))
        return fail(c, "'%s' does not exist", p);
    wchar_t *w = utf8_to_wide(full);
    HINSTANCE r = ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
    if ((INT_PTR)r <= 32)
        return fail(c, "Windows could not open '%s' (code %d)", full, (int)(INT_PTR)r);
    out(c, "Opened %s with the default application.\n", full);
    return 0;
}

static int a_get_recent_errors(tool_ctx *c)
{
    stats_recent_errors(&c->out, (int)arg_i(c, "count", 10));
    return 0;
}

static int a_get_stats(tool_ctx *c)
{
    stats_report(&c->out, (int)arg_i(c, "top_n", 20));
    return 0;
}

typedef LONG(WINAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);

static int a_get_system_info(tool_ctx *c)
{
    RTL_OSVERSIONINFOW v = { sizeof v };
    RtlGetVersionFn rtl = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rtl)
        rtl(&v);
    out(c, "OS: Windows %lu.%lu build %lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    wchar_t host[256];
    DWORD hn = 256;
    if (GetComputerNameW(host, &hn)) {
        char *h = wide_to_utf8(host);
        out(c, "Computer: %s\n", h ? h : "?");
        free(h);
    }
    DWORD release = 0, size = sizeof release;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\NET Framework Setup\\NDP\\v4\\Full", L"Release",
                     RRF_RT_REG_DWORD, NULL, &release, &size) == ERROR_SUCCESS)
        out(c, ".NET Framework: release %lu (%s)\n", release, release >= 528040 ? "4.8 or later" : "older than 4.8");
    char tia[64];
    tia_portal_version(tia, sizeof tia);
    out(c, "TIA Portal: %s\n", *tia ? tia : "V21 not found");
    wchar_t wdir[MAX_PATH];
    if (tia_find_openness_dir(wdir, MAX_PATH) == 0) {
        char *d = wide_to_utf8(wdir);
        out(c, "Openness: %s\n", d ? d : "?");
        free(d);
    }
    MEMORYSTATUSEX ms = { sizeof ms };
    GlobalMemoryStatusEx(&ms);
    char total[32], avail[32];
    fmt_size((long long)ms.ullTotalPhys, total, sizeof total);
    fmt_size((long long)ms.ullAvailPhys, avail, sizeof avail);
    out(c, "Memory: %s total, %s available\n", total, avail);
    out(c, "Server uptime: %lld s\n", stats_uptime_seconds());
    return 0;
}

static int a_get_version(tool_ctx *c)
{
    int n = 0;
    long long total = export_total_size(&n);
    char sz[32];
    fmt_size(total, sz, sizeof sz);
    out(c, "tiaComandante %s (%d tools, %d actions)\n", TC_VERSION, registry_count(), registry_action_total());
    out(c, "Data folder: %s\nConfig: %s\nExport store: %d entries, %s\n", g_cfg.data_dir, g_cfg.config_file, n, sz);
    return 0;
}

static int credential_args(tool_ctx *c, cred_kind *kind, const char **key)
{
    const char *k = arg_req(c, "kind");
    if (!k)
        return -1;
    if (cred_kind_parse(k, kind) != 0)
        return fail(c, "kind must be umac (TIA project user management) or plc (PLC password)");
    *key = arg_s(c, "key");
    if (!*key || !**key)
        *key = "*";
    return 0;
}

static int a_set_credential(tool_ctx *c)
{
    cred_kind kind;
    const char *key;
    if (credential_args(c, &kind, &key) != 0)
        return -1;
    char msg[1200], err[256];
    if (kind == CRED_UMAC)
        snprintf(msg, sizeof msg, "TIA Portal user management (UMAC) credentials for project:\n%s\n\nUser type: %s",
                 strcmp(key, "*") == 0 ? "(any project)" : key, arg_b(c, "global", 0) ? "Global (UMC)" : "Project user");
    else
        snprintf(msg, sizeof msg,
                 "PLC credentials for %s\n\nAccess-level password only: user name \"-\".\nPLC user management: the PLC user "
                 "(%s).",
                 strcmp(key, "*") == 0 ? "any PLC" : key, arg_b(c, "global", 0) ? "global user" : "project user");
    int rc = cred_prompt_store(kind, key, arg_b(c, "global", 0), msg, err, sizeof err);
    if (rc == 1)
        return fail(c, "the user cancelled the credential dialog");
    if (rc != 0)
        return fail(c, "%s", err);
    out(c, "Credential stored in the Windows Credential Manager as tiaComandante/%s/%s. The password was entered only in the "
           "Windows dialog.\n",
        cred_kind_name(kind), key);
    return 0;
}

static int a_list_credentials(tool_ctx *c)
{
    out(c, "Stored credentials (Windows Credential Manager, secrets never shown):\n");
    cred_list(&c->out);
    return 0;
}

static int a_delete_credential(tool_ctx *c)
{
    cred_kind kind;
    const char *key;
    if (credential_args(c, &kind, &key) != 0)
        return -1;
    if (cred_delete(kind, key) != 0)
        return fail(c, "no stored credential tiaComandante/%s/%s", cred_kind_name(kind), key);
    out(c, "Credential tiaComandante/%s/%s deleted.\n", cred_kind_name(kind), key);
    return 0;
}

static int a_search_device_catalog(tool_ctx *c)
{
    const char *filter = arg_s(c, "filter");
    int limit = (int)arg_i(c, "limit", 50);
    if (limit < 1)
        limit = 1;
    strbuf sb;
    sb_init(&sb);
    char info[256];
    int n = dc_catalog_search(filter, limit, &sb, info, sizeof info);
    if (n < 0) {
        sb_free(&sb);
        return fail(c, "no local hardware catalog yet: connect to TIA Portal and run hardware action=dump_catalog once "
                       "(or use hardware action=search_catalog for a live search)");
    }
    out_raw(c, sb_str(&sb));
    sb_free(&sb);
    out(c, "%d match(es)%s. Source:%s\n", n, n > limit ? " (refine the filter or raise limit)" : "", info);
    return 0;
}

static int a_reset_device_catalog(tool_ctx *c)
{
    dc_catalog_delete();
    if (!session_portal()) {
        out(c, "Local hardware catalog deleted. Connect to TIA Portal and run hardware action=dump_catalog to rebuild it.\n");
        return 0;
    }
    out(c, "Local hardware catalog deleted; reading it again from TIA Portal.\n");
    return hw_dump_catalog(c);
}

static int a_get_device_profiles(tool_ctx *c)
{
    strbuf sb;
    sb_init(&sb);
    int n = dc_profiles_list(&sb);
    out_raw(c, sb_str(&sb));
    sb_free(&sb);
    out(c, "%d device profile(s) (CPUs and interface modules seen by the hardware tool).\n", n);
    return 0;
}

static const action_def actions[] = {
    { "clear_exports", "optional olderThanHours=24", "Delete expired exports.", a_clear_exports, 0 },
    { "delete_credential", "kind=umac|plc; optional key", "Delete a stored credential.", a_delete_credential, 0 },
    { "list_credentials", "", "List stored credentials (kind, key, user, type). Secrets are never shown.",
      a_list_credentials, 0 },
    { "set_credential", "kind=umac|plc; optional key (project path or PLC IP, default * = any), global=false",
      "Ask the USER for credentials in the standard Windows credential dialog on this PC and store them in the Windows "
      "Credential Manager. umac: TIA project user management login used by session open for protected projects "
      "(global=true for UMC users). plc: PLC access password (user name \"-\") or PLC user (global=true for global "
      "users) used by go_online, compare_online_offline, download and upload. Never ask the user to type a password in "
      "the chat.",
      a_set_credential, 0 },
    { "delete_export", "exportId", "Delete a single export.", a_delete_export, 0 },
    { "get_export", "exportId; optional offset, length, raw=false",
      "Retrieve export content with paging. raw=true returns content only, no metadata header.", a_get_export, 0 },
    { "get_device_profiles", "",
      "All distinct CPUs and interface modules (order number, firmware, TIA version, first/last seen) met by the hardware "
      "tool.",
      a_get_device_profiles, 0 },
    { "get_recent_errors", "optional count=10", "Last N failed tool calls.", a_get_recent_errors, 0 },
    { "get_stats", "optional top_n=20", "Call statistics per tool+action, sorted by total calls.", a_get_stats, 0 },
    { "get_system_info", "", "OS, .NET Framework, TIA Portal version, Openness path, memory, uptime.", a_get_system_info, 0 },
    { "get_version", "", "Server version, data folder, export store size.", a_get_version, 0 },
    { "list_exports", "optional tool, limit=20", "Recent export results kept in the export store.", a_list_exports, 0 },
    { "open_file", "filePath",
      "Open a file or folder with the Windows default application. Use after an export when the user wants to inspect "
      "the result.",
      a_open_file, 0 },
    { "reset_device_catalog", "",
      "Delete the local copy of the hardware catalog and read it again from TIA Portal when connected.",
      a_reset_device_catalog, 0 },
    { "save_export", "exportId, outputPath",
      "Save export content to a file (outputPath may be a folder). Afterwards ask the user whether to open it "
      "(action=open_file).",
      a_save_export, 0 },
    { "search_device_catalog", "optional filter, limit=50",
      "Search the local copy of the TIA hardware catalog (made by hardware action=dump_catalog) - no TIA Portal needed. "
      "Every word of filter must match (article number, type name, version, catalog path or description).",
      a_search_device_catalog, 0 },
};

const tool_def tool_admin = {
    .name = "admin",
    .title = "Server administration",
    .summary = "Server administration: export store (exports referenced by exportId, kept 24 h), statistics, recent "
               "errors, system information, opening files, credentials, local hardware catalog and device profiles. Needs no TIA "
               "Portal connection.",
    .properties = "{"
                  "\"exportId\":{\"type\":\"string\",\"description\":\"Export identifier returned by an export action.\"},"
                  "\"offset\":{\"type\":\"integer\",\"description\":\"get_export: byte offset (default 0).\"},"
                  "\"length\":{\"type\":\"integer\",\"description\":\"get_export: bytes to return (default 60000).\"},"
                  "\"raw\":{\"type\":\"boolean\",\"description\":\"get_export: content only, without header.\"},"
                  "\"olderThanHours\":{\"type\":\"number\",\"description\":\"clear_exports: age threshold (default 24).\"},"
                  "\"tool\":{\"type\":\"string\",\"description\":\"list_exports: filter by tool name.\"},"
                  "\"limit\":{\"type\":\"integer\",\"description\":\"list_exports (default 20), search_device_catalog "
                  "(default 50): maximum entries.\"},"
                  "\"filter\":{\"type\":\"string\",\"description\":\"search_device_catalog: words to match.\"},"
                  "\"outputPath\":{\"type\":\"string\",\"description\":\"save_export: target file or folder.\"},"
                  "\"filePath\":{\"type\":\"string\",\"description\":\"open_file: file or folder to open.\"},"
                  "\"count\":{\"type\":\"integer\",\"description\":\"get_recent_errors: number of entries (default 10).\"},"
                  "\"top_n\":{\"type\":\"integer\",\"description\":\"get_stats: number of entries (default 20).\"},"
                  "\"kind\":{\"type\":\"string\",\"enum\":[\"umac\",\"plc\"],\"description\":\"Credential kind.\"},"
                  "\"key\":{\"type\":\"string\",\"description\":\"umac: project file path; plc: PLC IP address; default * = any.\"},"
                  "\"global\":{\"type\":\"boolean\",\"description\":\"umac: global (UMC) user instead of a project user.\"}"
                  "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
    .hints = 0,
};
