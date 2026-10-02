/* download_upload: compile-checked download to the PLC and station upload, with
   explicit confirmation gates. Download/upload confirmations raised by TIA
   Portal are answered by a policy implemented here in C. */
#include "tools.h"

#include "tia/online.h"
#include "tia/session.h"
#include "tia/tia_dyn.h"
#include "tia/tia_nav.h"
#include "tia/tia_sw.h"
#include "util/strbuf.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIRM_DOWNLOAD "I understand this will modify the PLC"
#define CONFIRM_UPLOAD "I understand this will add a device to the project"

/* ---- pre-flight ----------------------------------------------------------------------- */

typedef struct gates {
    int has_plc, configured, compiled, compile_errors, online;
    char state[32];
    char ip[64];
} gates;

static void run_gates(tool_ctx *c, nav_plc *plc, gates *g, int compile)
{
    memset(g, 0, sizeof *g);
    g->has_plc = plc->software != 0;
    th op = on_online_provider(plc);
    char *st = op ? td_get_s(op, "State") : NULL;
    snprintf(g->state, sizeof g->state, "%s", st ? st : "?");
    free(st);
    g->online = strcmp(g->state, "Online") == 0;
    th cfg = op ? td_get_h(op, "Configuration") : 0;
    if (cfg)
        td_get_b(cfg, "IsConfigured", &g->configured);
    on_device_ip(plc, g->ip, sizeof g->ip, NULL);
    td_clear_err();
    if (compile && !g->online) {
        th comp = td_service(plc->software, "Siemens.Engineering.Compiler.ICompilable");
        th res = comp ? td_call_h(comp, "Compile", NULL) : 0;
        if (res) {
            long long errs = 0;
            td_get_i(res, "ErrorCount", &errs);
            g->compiled = 1;
            g->compile_errors = (int)errs;
        }
        td_clear_err();
    }
    (void)c;
}

static int a_download_check(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    gates g;
    run_gates(c, &plc, &g, 1);
    out(c, "Device %s (PLC %s), configured IP %s, connection state %s\n", plc.device_name, plc.plc_name, *g.ip ? g.ip : "none",
        g.state);
    out(c, "[%s] PLC software present\n", g.has_plc ? "PASS" : "FAIL");
    const char *tip = arg_s(c, "targetIp");
    int conn_ok = g.configured || (tip && *tip);
    if (g.configured)
        out(c, "[PASS] online connection configured\n");
    else if (conn_ok)
        out(c, "[PASS] connection through targetIp %s (IP set at the device)\n", tip);
    else
        out(c, "[FAIL] online connection configured -> diagnostics action=configure_connection%s\n",
            *g.ip ? "" : ", or pass targetIp (the project has no IP for this PLC)");
    if (g.compiled)
        out(c, "[%s] compilation: %d error(s)%s\n", g.compile_errors ? "FAIL" : "PASS", g.compile_errors,
            g.compile_errors ? " -> blocks_read action=get_compiler_errors" : "");
    else
        out(c, "[SKIP] compilation not verified%s\n", g.online ? " (the device is online)" : "");
    const char *verdict = !g.has_plc || !conn_ok || (g.compiled && g.compile_errors) ? "NOT READY"
                          : !g.compiled                                                   ? "COMPILE UNVERIFIED"
                                                                                          : "READY";
    out(c, "Verdict: %s\n", verdict);
    return 0;
}

/* ---- download policy ------------------------------------------------------------------- */

typedef struct dl_policy {
    int stop_modules, start_after, reinit_db, hardware;
    int unhandled;
    SRWLOCK lock;
    strbuf log;
} dl_policy;

static void policy_log(dl_policy *p, const char *phase, const char *type, const char *choice, const char *msg)
{
    AcquireSRWLockExclusive(&p->lock);
    sb_printf(&p->log, "  [%s] %s -> %s%s%s\n", phase, type, choice, msg && *msg ? " (" : "", msg && *msg ? msg : "");
    if (msg && *msg)
        sb_append(&p->log, "");
    ReleaseSRWLockExclusive(&p->lock);
}

/* Sets CurrentSelection to the first accepted value. */
static const char *select_first(th cfg, const char *const *values)
{
    for (; *values; values++) {
        if (td_set(cfg, "CurrentSelection", cJSON_CreateString(*values)) == 0)
            return *values;
    }
    td_clear_err();
    return NULL;
}

static int on_download_config(void *ctx, const cJSON *args, cJSON **result, const char *phase)
{
    (void)result;
    dl_policy *p = ctx;
    th cfg = tdv_h(cJSON_GetArrayItem(args, 0));
    char *full = cfg ? td_typename(cfg) : NULL;
    const char *type = full ? (strrchr(full, '.') ? strrchr(full, '.') + 1 : full) : "?";
    char *msg = cfg ? td_get_s(cfg, "Message") : NULL;
    td_clear_err();
    const char *choice = NULL;
    if (strcmp(type, "StopModules") == 0) {
        static const char *stop[] = { "StopAll", NULL }, *keep[] = { "NoAction", NULL };
        choice = select_first(cfg, p->stop_modules ? stop : keep);
    } else if (strcmp(type, "StartModules") == 0 || strcmp(type, "StartBackupModules") == 0) {
        static const char *start[] = { "StartModule", NULL }, *keep[] = { "NoAction", NULL };
        choice = select_first(cfg, p->start_after ? start : keep);
    } else if (strcmp(type, "DataBlockReinitialization") == 0 || strcmp(type, "DataBlockReinitializationOrKeepActualValues") == 0) {
        static const char *reinit[] = { "StopPlcAndReinitialize", NULL };
        static const char *keep[] = { "KeepActualValues", "NoAction", NULL };
        choice = select_first(cfg, p->reinit_db ? reinit : keep);
    } else if (strcmp(type, "OverwriteSystemData") == 0) {
        static const char *ow[] = { "Overwrite", NULL }, *keep[] = { "NoAction", NULL };
        choice = select_first(cfg, p->hardware ? ow : keep);
    } else if (strcmp(type, "ConsistentBlocksDownload") == 0) {
        static const char *v[] = { "ConsistentDownload", NULL };
        choice = select_first(cfg, v);
    } else if (strcmp(type, "AllBlocksDownload") == 0) {
        static const char *v[] = { "DownloadAllBlocks", NULL };
        choice = select_first(cfg, v);
    } else if (strcmp(type, "AlarmTextLibrariesDownload") == 0 || strcmp(type, "OverwriteTargetLanguages") == 0) {
        static const char *v[] = { "ConsistentDownload", "Download", "Overwrite", NULL };
        choice = select_first(cfg, v);
    } else if (cfg && td_is(cfg, "Siemens.Engineering.Download.Configurations.DownloadCheckConfiguration")) {
        /* Plain acknowledgements ("I have read the message"). */
        if (td_set(cfg, "Checked", cJSON_CreateBool(1)) == 0)
            choice = "Checked";
        td_clear_err();
    }
    if (!choice) {
        InterlockedIncrement((volatile LONG *)&p->unhandled);
        policy_log(p, phase, type, "NOT HANDLED - the download is cancelled by TIA Portal", msg);
    } else {
        policy_log(p, phase, type, choice, msg);
    }
    free(full);
    free(msg);
    return 0;
}

static int on_pre(void *ctx, const cJSON *args, cJSON **result) { return on_download_config(ctx, args, result, "pre"); }
static int on_post(void *ctx, const cJSON *args, cJSON **result) { return on_download_config(ctx, args, result, "post"); }

static void print_result_messages(tool_ctx *c, th messages, int depth)
{
    if (!messages || depth > 8)
        return;
    cJSON *list = td_enum(messages, "Message,State", 300);
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        const char *m = tdi_s(it, "Message");
        if (m && *m)
            out(c, "%*s[%s] %s\n", depth * 2, "", tdi_s(it, "State") ? tdi_s(it, "State") : "?", m);
        print_result_messages(c, td_get_h(tdv_h(it), "Messages"), depth + 1);
    }
    cJSON_Delete(list);
}

static const char *download_options(const char *mode, int *hardware)
{
    *hardware = 0;
    if (!mode || !*mode || _stricmp(mode, "software_changes") == 0)
        return "SoftwareOnlyChanges";
    if (_stricmp(mode, "software") == 0)
        return "Software";
    if (_stricmp(mode, "hardware_software") == 0) {
        *hardware = 1;
        return "Hardware, Software";
    }
    if (_stricmp(mode, "hardware") == 0) {
        *hardware = 1;
        return "Hardware";
    }
    return NULL;
}

static int a_download_to_device(tool_ctx *c)
{
    nav_plc plc;
    if (sw_plc(c, &plc) != 0)
        return -1;
    if (!confirmed(c, CONFIRM_DOWNLOAD))
        return fail(c, "DESTRUCTIVE: this downloads to the real PLC %s. Repeat with confirm='%s'.", plc.device_name,
                    CONFIRM_DOWNLOAD);
    int hardware = 0;
    const char *options = download_options(arg_s(c, "mode"), &hardware);
    if (!options)
        return fail(c, "mode must be software_changes, software, hardware_software or hardware");

    gates g;
    run_gates(c, &plc, &g, 1);
    if (g.compiled && g.compile_errors)
        return fail(c, "the software has %d compile error(s): fix them first (blocks_read action=get_compiler_errors)",
                    g.compile_errors);

    th dp = on_download_provider(&plc);
    th cfg = dp ? td_get_h(dp, "Configuration") : 0;
    if (!cfg)
        return fail_td(c, "no download provider for this device");
    const char *pc = arg_s(c, "pcInterfaceName");
    th pc_h = 0;
    th target = on_find_target(c, cfg, arg_s(c, "connectionMode"), pc, arg_s(c, "targetInterface"), &pc_h);
    if (!target)
        return -1;

    dl_policy *p = calloc(1, sizeof *p);
    if (!p)
        return fail(c, "out of memory");
    InitializeSRWLock(&p->lock);
    sb_init(&p->log);
    p->stop_modules = arg_b(c, "stopModules", 0);
    p->start_after = arg_b(c, "startAfterDownload", 1);
    p->reinit_db = arg_b(c, "reinitializeDataBlocks", 0);
    p->hardware = hardware;
    long long cb_pre = td_register_callback(on_pre, p);
    long long cb_post = td_register_callback(on_post, p);

    /* Target address: explicit targetIp, otherwise the project IP. */
    const char *ip = arg_s(c, "targetIp");
    if (!ip || !*ip)
        ip = g.ip;
    th addr = 0;
    if (ip && *ip) {
        th addrs = td_get_h(target, "Addresses");
        addr = addrs ? td_call_h(addrs, "Find", tda("s", ip)) : 0;
        td_clear_err();
        if (!addr && addrs)
            addr = td_call_h(addrs, "Create", tda("s", ip));
        td_clear_err();
    }
    progress(c, 0, 0, "downloading");
    cJSON *res;
    if (addr)
        res = td_call(dp, "Download", tda("hhcce", target, addr, cb_pre, cb_post, "Siemens.Engineering.Download.DownloadOptions", options));
    else
        res = td_call(dp, "Download", tda("hcce", target, cb_pre, cb_post, "Siemens.Engineering.Download.DownloadOptions", options));
    td_unregister_callback(cb_pre);
    td_unregister_callback(cb_post);

    int rc = 0;
    if (!res) {
        rc = fail_td(c, "download failed");
    } else {
        th result = tdv_h(res);
        cJSON *a = td_attrs(result, "State,ErrorCount,WarningCount");
        out(c, "Download (%s) to %s: %s (errors: %lld, warnings: %lld)\n", options, plc.device_name,
            tdv_s(cJSON_GetObjectItemCaseSensitive(a, "State")) ? tdv_s(cJSON_GetObjectItemCaseSensitive(a, "State")) : "?",
            tdv_i(cJSON_GetObjectItemCaseSensitive(a, "ErrorCount"), 0), tdv_i(cJSON_GetObjectItemCaseSensitive(a, "WarningCount"), 0));
        if (tdv_i(cJSON_GetObjectItemCaseSensitive(a, "ErrorCount"), 0) > 0)
            c->is_error = 1;
        cJSON_Delete(a);
        print_result_messages(c, td_get_h(result, "Messages"), 1);
        cJSON_Delete(res);
    }
    if (p->log.len)
        out(c, "Download dialogs answered:\n%s", sb_str(&p->log));
    if (p->unhandled)
        out(c, "%ld confirmation(s) had no safe automatic answer: complete the download in TIA Portal or adjust the options.\n",
            (long)p->unhandled);
    sb_free(&p->log);
    free(p);
    return rc ? rc : (c->is_error ? -1 : 0);
}

/* ---- upload ------------------------------------------------------------------------------- */

static int a_upload_check(tool_ctx *c)
{
    th up = td_service(session_project(), "Siemens.Engineering.Upload.StationUploadProvider");
    if (!up)
        return fail(c, "NOT READY: station upload is not available in this project (%s)", td_err());
    th cfg = td_get_h(up, "Configuration");
    out(c, "Station upload available. Connection modes and PG/PC interfaces:\n");
    on_describe_interfaces(c, cfg);
    out(c, "Verdict: READY (use diagnostics action=scan_devices to find the station address, then upload_station)\n");
    return 0;
}

static int on_upload_config(void *ctx, const cJSON *args, cJSON **result)
{
    (void)result;
    strbuf *log = ctx;
    th cfg = tdv_h(cJSON_GetArrayItem(args, 0));
    char *full = cfg ? td_typename(cfg) : NULL;
    char *msg = cfg ? td_get_s(cfg, "Message") : NULL;
    sb_printf(log, "  %s%s%s\n", full ? full : "?", msg ? ": " : "", msg ? msg : "");
    free(full);
    free(msg);
    td_clear_err();
    return 0;
}

static int a_upload_station(tool_ctx *c)
{
    if (!confirmed(c, CONFIRM_UPLOAD))
        return fail(c, "DESTRUCTIVE TO THE PROJECT: the uploaded station is added as a new device. Repeat with confirm='%s'.",
                    CONFIRM_UPLOAD);
    const char *mode = arg_req(c, "modeName");
    const char *pc = mode ? arg_req(c, "pcInterfaceName") : NULL;
    if (!pc)
        return -1;
    if (arg_s(c, "readPassword") || arg_s(c, "writePassword"))
        return fail(c, "password-protected uploads are not supported yet");
    th up = td_service(session_project(), "Siemens.Engineering.Upload.StationUploadProvider");
    th cfg = up ? td_get_h(up, "Configuration") : 0;
    if (!cfg)
        return fail_td(c, "station upload is not available");
    th pc_h = 0;
    tool_ctx probe;
    ctx_init(&probe, c->tool, NULL);
    on_find_target(&probe, cfg, mode, pc, NULL, &pc_h); /* upload PG/PC interfaces have no target interfaces */
    if (!pc_h) {
        out_raw(c, sb_str(&probe.out));
        ctx_free(&probe);
        return fail(c, "PG/PC interface '%s' not found in mode %s", pc, mode);
    }
    ctx_free(&probe);
    /* Address: explicit targetIp or the addressIndex-th accessible device. */
    const char *ip = arg_s(c, "targetIp");
    char found_ip[64] = "";
    if (!ip || !*ip) {
        cJSON *devs = td_call(pc_h, "GetAccessibleDevices", NULL);
        cJSON *list = devs && tdv_h(devs) ? td_enum(tdv_h(devs), "Name,Address", -1) : NULL;
        int idx = (int)arg_i(c, "addressIndex", 0);
        const cJSON *d = cJSON_GetArrayItem(list, idx);
        if (d && tdi_s(d, "Address"))
            snprintf(found_ip, sizeof found_ip, "%s", tdi_s(d, "Address"));
        cJSON_Delete(list);
        cJSON_Delete(devs);
        if (!found_ip[0])
            return fail(c, "no accessible device at addressIndex %d on '%s' (diagnostics action=scan_devices)", idx, pc);
        ip = found_ip;
    }
    th addrs = td_get_h(pc_h, "Addresses");
    th addr = addrs ? td_call_h(addrs, "Find", tda("s", ip)) : 0;
    td_clear_err();
    if (!addr && addrs)
        addr = td_call_h(addrs, "Create", tda("s", ip));
    if (!addr)
        return fail_td(c, "cannot create the upload address");
    strbuf log;
    sb_init(&log);
    long long cb = td_register_callback(on_upload_config, &log);
    progress(c, 0, 0, "uploading station");
    th res = td_call_h(up, "StationUpload", tda("hc", addr, cb));
    td_unregister_callback(cb);
    int rc = 0;
    if (!res) {
        rc = fail_td(c, "station upload failed");
    } else {
        cJSON *a = td_attrs(res, "State,ErrorCount,WarningCount");
        th station = td_get_h(res, "UploadedStation");
        char *name = station ? td_get_s(station, "Name") : NULL;
        out(c, "Upload from %s: %s (errors: %lld, warnings: %lld). New device: %s\n", ip,
            tdv_s(cJSON_GetObjectItemCaseSensitive(a, "State")) ? tdv_s(cJSON_GetObjectItemCaseSensitive(a, "State")) : "?",
            tdv_i(cJSON_GetObjectItemCaseSensitive(a, "ErrorCount"), 0), tdv_i(cJSON_GetObjectItemCaseSensitive(a, "WarningCount"), 0),
            name ? name : "(none)");
        free(name);
        cJSON_Delete(a);
        print_result_messages(c, td_get_h(res, "Messages"), 1);
    }
    if (log.len)
        out(c, "Upload dialogs (left to TIA Portal):\n%s", sb_str(&log));
    sb_free(&log);
    return rc;
}

static const action_def actions[] = {
    { "download_check", "deviceName; optional targetIp",
      "Silent pre-flight: call BEFORE any download or go_online. Checks PLC software, connection configuration and "
      "compilation. Verdicts: READY, NOT READY (a gate failed - relay it), COMPILE UNVERIFIED (device online, compilation "
      "not checked - not a pass).",
      a_download_check, AF_PROJECT },
    { "download_to_device",
      "deviceName, confirm, pcInterfaceName; optional mode=software_changes|software|hardware_software|hardware, "
      "stopModules=false, startAfterDownload=true, reinitializeDataBlocks=false, targetIp, targetInterface",
      "DESTRUCTIVE. Mandatory compile pre-check. confirm='I understand this will modify the PLC'. pcInterfaceName selects "
      "the network adapter (e.g. 'PLCSIM'). TIA Portal download dialogs are answered from stopModules/startAfterDownload/"
      "reinitializeDataBlocks; any other question cancels the download.",
      a_download_to_device, AF_PROJECT | AF_WRITES | AF_DESTRUCTIVE },
    { "upload_check", "", "Silent pre-flight before an upload: station upload availability and PG/PC interfaces.",
      a_upload_check, AF_PROJECT },
    { "upload_station", "modeName, pcInterfaceName, confirm; optional addressIndex=0, targetIp",
      "DESTRUCTIVE TO PROJECT (adds a device). confirm='I understand this will add a device to the project'. The station "
      "address is targetIp or the addressIndex-th device found on the interface.",
      a_upload_station, AF_PROJECT | AF_WRITES | AF_DESTRUCTIVE },
};

const tool_def tool_download_upload = {
    .name = "download_upload",
    .title = "Download / upload",
    .summary = "Compile-checked download to a PLC and station upload, with explicit confirmation phrases. Always run "
               "download_check / upload_check first.",
    .properties =
        "{"
        "\"deviceName\":{\"type\":\"string\",\"description\":\"Device or PLC name (session list_devices).\"},"
        "\"confirm\":{\"type\":\"string\",\"description\":\"Exact confirmation phrase (see the action).\"},"
        "\"mode\":{\"type\":\"string\",\"enum\":[\"software_changes\",\"software\",\"hardware_software\",\"hardware\"]},"
        "\"stopModules\":{\"type\":\"boolean\"},\"startAfterDownload\":{\"type\":\"boolean\"},"
        "\"reinitializeDataBlocks\":{\"type\":\"boolean\"},"
        "\"pcInterfaceName\":{\"type\":\"string\"},\"targetInterface\":{\"type\":\"string\"},\"targetIp\":{\"type\":\"string\"},"
        "\"connectionMode\":{\"type\":\"string\",\"description\":\"download: connection mode, default PN/IE.\"},"
        "\"modeName\":{\"type\":\"string\",\"description\":\"upload_station: connection mode, e.g. PN/IE.\"},"
        "\"addressIndex\":{\"type\":\"integer\"},"
        "\"readPassword\":{\"type\":\"string\"},\"writePassword\":{\"type\":\"string\"}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
    .hints = TH_DESTRUCTIVE,
};
