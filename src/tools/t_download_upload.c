/* download_upload: compile-checked download to the PLC and station upload, with
   explicit confirmation gates. Download/upload confirmations raised by TIA
   Portal are answered by a policy implemented here in C. */
#include "tools.h"

#include "tia/online.h"
#include "tia/session.h"
#include "tia/tia_dyn.h"
#include "tia/tia_nav.h"
#include "tia/tia_sw.h"
#include "util/log.h"
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
    int hardware; /* the compile covered the whole device (hardware and software) */
    th compile_result;
    char state[32];
    char ip[64];
} gates;

static const char *download_options(const char *mode, int *hardware);

/* hardware: compile the device (hardware configuration and software), as a hardware download does;
   otherwise only the PLC software. */
static void run_gates(tool_ctx *c, nav_plc *plc, gates *g, int compile, int hardware)
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
        g->hardware = hardware && plc->device;
        th comp = td_service(g->hardware ? plc->device : plc->software, "Siemens.Engineering.Compiler.ICompilable");
        th res = comp ? session_compile(comp) : 0;
        if (res) {
            long long errs = 0;
            td_get_i(res, "ErrorCount", &errs);
            g->compiled = 1;
            g->compile_errors = (int)errs;
            g->compile_result = res;
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
    int hardware = 0;
    if (arg_s(c, "mode") && !download_options(arg_s(c, "mode"), &hardware))
        return fail(c, "mode must be software_changes, software, hardware_software or hardware");
    gates g;
    run_gates(c, &plc, &g, 1, hardware);
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
    if (g.compiled) {
        out(c, "[%s] compilation (%s): %d error(s)%s\n", g.compile_errors ? "FAIL" : "PASS",
            g.hardware ? "hardware and software" : "software", g.compile_errors,
            g.compile_errors ? (g.hardware ? ":" : " -> blocks_read action=get_compiler_errors") : "");
        if (g.compile_errors && g.hardware)
            compile_list_errors(c, g.compile_result, 10);
    } else
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
    on_secret secret;
} dl_policy;

static void policy_log(dl_policy *p, const char *phase, const char *type, const char *choice, const char *msg)
{
    /* TIA messages are multi-line: keep one line per answered dialog. */
    char flat[512] = "";
    size_t n = 0;
    for (const char *s = msg ? msg : ""; *s && n < sizeof flat - 1; s++)
        flat[n++] = (*s == '\r' || *s == '\n') ? ' ' : *s;
    flat[n] = 0;
    AcquireSRWLockExclusive(&p->lock);
    sb_printf(&p->log, "  [%s] %s -> %s%s%s%s\n", phase, type, choice, *flat ? " (" : "", flat, *flat ? ")" : "");
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
    /* The "keep running" answers (NoAction, KeepActualValues) are not offered when the download cannot do
       without them: say which option allows it. */
    const char *refused = NULL;
    if (!choice && strcmp(type, "StopModules") == 0 && !p->stop_modules)
        refused = "NOT ALLOWED - this download needs the CPU in STOP: repeat with stopModules=true (download cancelled)";
    else if (!choice && strncmp(type, "DataBlockReinitialization", 25) == 0 && !p->reinit_db)
        refused = "NOT ALLOWED - this download reinitializes data blocks (actual values back to start values, CPU "
                  "stopped): repeat with reinitializeDataBlocks=true (download cancelled)";
    const char *pw_note = NULL;
    if (!choice && !refused && on_answer_password(&p->secret, cfg, &pw_note) == 0)
        choice = pw_note;
    if (!choice) {
        InterlockedIncrement((volatile LONG *)&p->unhandled);
        policy_log(p, phase, type, refused ? refused : pw_note ? pw_note : "NOT HANDLED - the download is cancelled by TIA Portal", msg);
    } else {
        policy_log(p, phase, type, choice, msg);
    }
    free(full);
    free(msg);
    return 0;
}

static int on_pre(void *ctx, const cJSON *args, cJSON **result) { return on_download_config(ctx, args, result, "pre"); }
static int on_post(void *ctx, const cJSON *args, cJSON **result) { return on_download_config(ctx, args, result, "post"); }

/* Prints download/upload result messages. Non-success messages are always
   shown; plain "Success" lines are capped (an upload lists every block). */
typedef struct msg_budget {
    int success_shown;
    int success_hidden;
} msg_budget;

static void print_messages_rec(tool_ctx *c, th messages, int depth, msg_budget *b)
{
    if (!messages || depth > 8)
        return;
    cJSON *list = td_enum(messages, "Message,State", 2000);
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        const char *m = tdi_s(it, "Message");
        const char *st = tdi_s(it, "State");
        if (m && *m) {
            int plain_success = st && strcmp(st, "Success") == 0;
            if (!plain_success || b->success_shown < 15) {
                out(c, "%*s[%s] %s\n", depth * 2, "", st ? st : "?", m);
                if (plain_success)
                    b->success_shown++;
            } else {
                b->success_hidden++;
            }
        }
        print_messages_rec(c, td_get_h(tdv_h(it), "Messages"), depth + 1, b);
    }
    cJSON_Delete(list);
}

static void print_result_messages(tool_ctx *c, th messages, int depth)
{
    msg_budget b = { 0, 0 };
    print_messages_rec(c, messages, depth, &b);
    if (b.success_hidden)
        out(c, "%*s... and %d more [Success] message(s)\n", depth * 2, "", b.success_hidden);
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

/* A download leaves TIA Portal with a temporary online connection that the next
   go offline drops ("not configured"). Make the PG/PC interface just used the
   project's online connection, as configure_connection does. */
static void keep_connection(tool_ctx *c, const nav_plc *plc, const char *pc)
{
    th op = on_online_provider(plc);
    th cfg = op ? td_get_h(op, "Configuration") : 0;
    tool_ctx probe;
    ctx_init(&probe, c->tool, NULL);
    th ti = cfg ? on_find_target(&probe, cfg, arg_s(c, "connectionMode"), pc, arg_s(c, "targetInterface"), NULL) : 0;
    ctx_free(&probe);
    cJSON *ok = ti ? td_call(cfg, "ApplyConfiguration", tda("h", ti)) : NULL;
    if (cJSON_IsTrue(ok))
        out(c, "Online connection of %s set to PG/PC interface '%s' (as configure_connection).\n", plc->device_name, pc);
    else
        LOG_W("keeping the download connection of %s: %s", plc->device_name, td_err());
    cJSON_Delete(ok);
    td_clear_err();
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
    if (arg_s(c, "readPassword") || arg_s(c, "writePassword"))
        return fail(c, "%s", PLC_PASSWORD_ARGS);
    const char *options = download_options(arg_s(c, "mode"), &hardware);
    if (!options)
        return fail(c, "mode must be software_changes, software, hardware_software or hardware");

    gates g;
    run_gates(c, &plc, &g, 1, hardware);
    if (g.compiled && g.compile_errors && g.hardware) {
        fail(c, "the device has %d compile error(s) (hardware and software): fix them first", g.compile_errors);
        compile_list_errors(c, g.compile_result, 10);
        return -1;
    }
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
    on_secret_init(&p->secret, ip);
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
    cJSON *res = NULL;
    int legacy_prev;
    char err[1024] = "", etype[256] = "";
    if (on_apply_legacy(c, cfg, "the download", &legacy_prev) == 0) {
        /* Secure communication: the download connects like go_online and may ask to verify the PLC certificate. */
        on_legit legit;
        on_legitimation_begin(&legit, cfg, ip);
        legit.trust_certificate = arg_b(c, "trustPlcCertificate", 0);
        if (addr)
            res = td_call(dp, "Download", tda("hhcce", target, addr, cb_pre, cb_post, "Siemens.Engineering.Download.DownloadOptions", options));
        else
            res = td_call(dp, "Download", tda("hcce", target, cb_pre, cb_post, "Siemens.Engineering.Download.DownloadOptions", options));
        if (!res) {
            snprintf(err, sizeof err, "%s", td_err());
            snprintf(etype, sizeof etype, "%s", td_err_type());
        }
        on_legitimation_end(c, &legit);
        on_restore_legacy(cfg, legacy_prev);
        if (!res)
            td_set_err(etype, "%s", err);
    }
    td_unregister_callback(cb_pre);
    td_unregister_callback(cb_post);

    int rc = 0;
    int downloaded = res != NULL;
    if (!res && c->is_error) {
        rc = -1; /* legacyCommunication could not be applied */
    } else if (!res) {
        rc = fail_td(c, "download failed");
        if (strstr(td_err(), "connection"))
            on_legacy_hint(c);
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
    if (downloaded)
        keep_connection(c, &plc, pc);
    if (p->log.len)
        out(c, "Download dialogs answered:\n%s", sb_str(&p->log));
    if (p->unhandled)
        out(c, "%ld confirmation(s) had no safe automatic answer: complete the download in TIA Portal or adjust the options.\n",
            (long)p->unhandled);
    sb_free(&p->log);
    on_secret_free(&p->secret);
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

typedef struct up_ctx {
    strbuf log;
    on_secret secret;
} up_ctx;

static int on_upload_config(void *ctx, const cJSON *args, cJSON **result)
{
    (void)result;
    up_ctx *u = ctx;
    th cfg = tdv_h(cJSON_GetArrayItem(args, 0));
    char *full = cfg ? td_typename(cfg) : NULL;
    char *msg = cfg ? td_get_s(cfg, "Message") : NULL;
    td_clear_err();
    const char *note = NULL;
    on_answer_password(&u->secret, cfg, &note);
    sb_printf(&u->log, "  %s%s%s%s%s\n", full ? full : "?", msg ? ": " : "", msg ? msg : "", note ? " -> " : "",
              note ? note : "");
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
        return fail(c, "%s", PLC_PASSWORD_ARGS);
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
    up_ctx u;
    sb_init(&u.log);
    on_secret_init(&u.secret, ip);
    int legacy_prev;
    if (on_apply_legacy(c, cfg, "the upload", &legacy_prev) != 0) {
        sb_free(&u.log);
        on_secret_free(&u.secret);
        return -1;
    }
    long long cb = td_register_callback(on_upload_config, &u);
    progress(c, 0, 0, "uploading station");
    th res = td_call_h(up, "StationUpload", tda("hc", addr, cb));
    td_unregister_callback(cb);
    on_restore_legacy(cfg, legacy_prev);
    int rc = 0;
    if (!res) {
        rc = fail_td(c, "station upload failed");
        if (strstr(td_err(), "connection"))
            on_legacy_hint(c);
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
    if (u.log.len)
        out(c, "Upload dialogs:\n%s", sb_str(&u.log));
    sb_free(&u.log);
    on_secret_free(&u.secret);
    return rc;
}

static const action_def actions[] = {
    { "download_check", "deviceName; optional targetIp, mode",
      "Silent pre-flight: call BEFORE any download or go_online. Checks PLC software, connection configuration and "
      "compilation (with mode=hardware or hardware_software the whole device, hardware configuration included). Verdicts: READY, NOT READY (a gate failed - relay it), COMPILE UNVERIFIED (device online, compilation "
      "not checked - not a pass).",
      a_download_check, AF_PROJECT },
    { "download_to_device",
      "deviceName, confirm, pcInterfaceName; optional mode=software_changes|software|hardware_software|hardware, "
      "stopModules=false, startAfterDownload=true, reinitializeDataBlocks=false, targetIp, targetInterface, "
      "legacyCommunication, trustPlcCertificate",
      "DESTRUCTIVE. Mandatory compile pre-check. confirm='I understand this will modify the PLC'. pcInterfaceName selects "
      "the network adapter (e.g. 'PLCSIM'). TIA Portal download dialogs are answered from stopModules/startAfterDownload/"
      "reinitializeDataBlocks; any other question cancels the download. PLC access passwords come from the Windows "
      "Credential Manager (admin action=set_credential kind=plc key=<PLC IP>).",
      a_download_to_device, AF_PROJECT | AF_WRITES | AF_DESTRUCTIVE | AF_NO_TX },
    { "upload_check", "", "Silent pre-flight before an upload: station upload availability and PG/PC interfaces.",
      a_upload_check, AF_PROJECT },
    { "upload_station", "modeName, pcInterfaceName, confirm; optional addressIndex=0, targetIp, legacyCommunication",
      "DESTRUCTIVE TO PROJECT (adds a device). confirm='I understand this will add a device to the project'. The station "
      "address is targetIp or the addressIndex-th device found on the interface. PLC passwords: as for download_to_device.",
      a_upload_station, AF_PROJECT | AF_WRITES | AF_DESTRUCTIVE | AF_NO_TX },
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
        "\"legacyCommunication\":{\"type\":\"boolean\",\"description\":\"download_to_device, upload_station: use legacy "
        "(non-secure) PG/PC communication, if the CPU allows it.\"},"
        "\"trustPlcCertificate\":{\"type\":\"boolean\",\"description\":\"download_to_device: trust the PLC certificate when "
        "TIA Portal asks to verify it (secure PG/PC communication). Only after the user confirmed the PLC.\"},"
        "\"readPassword\":{\"type\":\"string\",\"description\":\"Not accepted: store PLC passwords with admin action=set_credential kind=plc.\"},"
        "\"writePassword\":{\"type\":\"string\",\"description\":\"Not accepted: store PLC passwords with admin action=set_credential kind=plc.\"}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
    .hints = TH_DESTRUCTIVE,
};
