#include "tools.h"

#include "app/config.h"
#include "mcp/registry.h"
#include "tc_version.h"
#include "tia/session.h"
#include "tia/tia_env.h"
#include "util/utf.h"

#include <windows.h>
#include <stdlib.h>

static int get_info(tool_ctx *c)
{
    char tia[64];
    wchar_t wdir[MAX_PATH];
    tia_portal_version(tia, sizeof tia);
    char *dir = tia_find_openness_dir(wdir, MAX_PATH) == 0 ? wide_to_utf8(wdir) : NULL;

    out(c, "tiaComandante %s - MCP server for Siemens TIA Portal V21 (Openness API)\n", TC_VERSION);
    out(c, "Tools: %d, actions: %d\n", registry_count(), registry_action_total());
    out(c, "TIA Portal: %s\n", *tia ? tia : "V21 not found in the registry");
    out(c, "Openness assemblies: %s\n", dir ? dir : "not found");
    if (session_bridge_ok())
        out(c, ".NET bridge: running\n");
    else if (*session_bridge_error())
        out(c, ".NET bridge: failed (%s)\n", session_bridge_error());
    else
        out(c, ".NET bridge: not started yet (starts on the first TIA Portal call)\n");
    if (session_portal())
        out(c, "Connection: TIA Portal pid %ld (%s, %s)\n", session_pid(), session_with_ui() ? "with UI" : "without UI",
            session_launched() ? "started by tiaComandante" : "attached");
    else
        out(c, "Connection: not connected\n");
    out(c, "Read-only mode: %s\n", config_read_only() ? "ON (write actions are refused)" : "off");
    out(c, "Config: %s\nData: %s\n", g_cfg.config_file, g_cfg.data_dir);
    out(c, "Workflow: session get_state -> session connect | open | create -> session list_devices -> "
           "blocks_read / tag / db / udt / ... with deviceName -> session save.\n");
    free(dir);
    return 0;
}

const tool_def tool_get_info = {
    .name = "get_info",
    .title = "Server information",
    .summary = "tiaComandante server information: version, tool count, TIA Portal V21/Openness detection, "
               "connection state and read-only mode. Needs no TIA Portal connection.",
    .properties = "{}",
    .direct = get_info,
    .hints = TH_READONLY | TH_IDEMPOTENT,
};
