#include "app/config.h"
#include "app/export_store.h"
#include "app/stats.h"
#include "mcp/server.h"
#include "selftest.h"
#include "tc_version.h"
#include "tia/session.h"
#include "tia/tia_dyn.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/utf.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr,
            "tiacomandante %s - MCP server for Siemens TIA Portal V21 (Openness)\n"
            "usage:\n"
            "  tiacomandante [options]                    run the MCP server on stdio\n"
            "  tiacomandante [options] --call TOOL [JSON] run one tool call and print the result\n"
            "  tiacomandante --selftest                   check CLR/bridge/Openness against a running TIA Portal\n"
            "  tiacomandante --members TYPE               list the members of an Openness type (development aid)\n"
            "  tiacomandante --credentials list | umac [PROJECT|*] [--global] | plc [IP|*] | delete umac|plc [KEY]\n"
            "                                             store credentials via the Windows credential dialog\n"
            "  tiacomandante --version\n"
            "options:\n"
            "  --read-only        refuse every action that modifies the project or the PLC\n"
            "  --log-level LEVEL  debug | info | warn | error (default info)\n",
            TC_VERSION);
}

static int parse_level(const char *s)
{
    if (_stricmp(s, "debug") == 0)
        return LOG_DEBUG;
    if (_stricmp(s, "warn") == 0)
        return LOG_WARN;
    if (_stricmp(s, "error") == 0)
        return LOG_ERROR;
    return LOG_INFO;
}

static int run(int argc, char **argv)
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);

    const char *mode = "";
    const char *call_tool = NULL, *call_args = NULL, *members_type = NULL;
    int read_only = 0, level = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--read-only") == 0) {
            read_only = 1;
        } else if (strcmp(a, "--log-level") == 0 && i + 1 < argc) {
            level = parse_level(argv[++i]);
        } else if (strcmp(a, "--call") == 0 && i + 1 < argc) {
            mode = a;
            call_tool = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-')
                call_args = argv[++i];
        } else if (strcmp(a, "--members") == 0 && i + 1 < argc) {
            mode = a;
            members_type = argv[++i];
        } else if (strcmp(a, "--version") == 0) {
            printf("tiacomandante %s\n", TC_VERSION);
            return 0;
        } else if (strcmp(a, "--selftest") == 0) {
            mode = a;
        } else if (strcmp(a, "--credentials") == 0) {
            /* --credentials list | umac [KEY] [--global] | plc [KEY] | delete umac|plc [KEY]
               mapped onto admin set/list/delete_credential. */
            static char json[1400];
            cJSON *args = cJSON_CreateObject();
            const char *sub = i + 1 < argc ? argv[++i] : "list";
            if (strcmp(sub, "delete") == 0) {
                cJSON_AddStringToObject(args, "action", "delete_credential");
                cJSON_AddStringToObject(args, "kind", i + 1 < argc ? argv[++i] : "");
            } else if (strcmp(sub, "umac") == 0 || strcmp(sub, "plc") == 0) {
                cJSON_AddStringToObject(args, "action", "set_credential");
                cJSON_AddStringToObject(args, "kind", sub);
            } else {
                cJSON_AddStringToObject(args, "action", "list_credentials");
            }
            while (i + 1 < argc) {
                const char *x = argv[++i];
                if (strcmp(x, "--global") == 0)
                    cJSON_AddBoolToObject(args, "global", 1);
                else
                    cJSON_AddStringToObject(args, "key", x);
            }
            char *s = cJSON_PrintUnformatted(args);
            snprintf(json, sizeof json, "%s", s ? s : "{}");
            cJSON_free(s);
            cJSON_Delete(args);
            mode = "--call";
            call_tool = "admin";
            call_args = json;
        } else {
            usage();
            return strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0 ? 0 : 2;
        }
    }

    config_load();
    if (read_only)
        g_cfg.read_only_forced = 1;
    log_set_level(level >= 0 ? level : g_cfg.log_level);
    char logs[TC_PATH_MAX];
    fs_join(logs, sizeof logs, g_cfg.data_dir, "logs");
    fs_mkdirs(logs);
    log_init(logs);
    stats_init();
    export_clear(24);

    int rc;
    if (!*mode) {
        rc = mcp_serve();
    } else {
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) {
            LOG_E("CoInitializeEx failed (0x%08lx)", (unsigned long)hr);
            return 1;
        }
        log_set_stderr(strcmp(mode, "--call") != 0 || level >= 0);
        if (strcmp(mode, "--call") == 0) {
            rc = mcp_call_once(call_tool, call_args);
        } else if (session_bridge(NULL) != 0) {
            fprintf(stderr, "%s\n", session_bridge_error());
            rc = 2;
        } else if (strcmp(mode, "--selftest") == 0) {
            rc = selftest_run();
        } else {
            cJSON *m = td_members(0, members_type);
            char *s = m ? cJSON_Print(m) : NULL;
            printf("%s\n", s ? s : td_err());
            rc = m ? 0 : 1;
            cJSON_free(s);
            cJSON_Delete(m);
        }
        CoUninitialize();
    }
    LOG_I("exit %d", rc);
    log_close();
    return rc;
}

/* Entry point called by the tiacomandante.exe launcher. Arguments are taken
   from the wide command line so that non-ASCII paths survive. */
__declspec(dllexport) int tc_main(void)
{
    int argc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv)
        return 3;
    char **argv = calloc((size_t)argc + 1, sizeof(char *));
    if (!argv)
        return 3;
    for (int i = 0; i < argc; i++) {
        argv[i] = wide_to_utf8(wargv[i]);
        if (!argv[i])
            argv[i] = _strdup("");
    }
    LocalFree(wargv);
    int rc = run(argc, argv);
    for (int i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
    return rc;
}
