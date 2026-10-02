/* TIA Portal connection state, owned by the worker thread. */
#ifndef TC_SESSION_H
#define TC_SESSION_H

#include "mcp/tool.h"
#include "tia/tia_dyn.h"

/* Lazily starts the .NET bridge; fails the call if it cannot start. */
int session_bridge(tool_ctx *c);
int session_bridge_ok(void);
const char *session_bridge_error(void);

/* Checks the AF_* requirements of an action (read-only mode, bridge,
   connection, open project, offline). May auto-attach to a running TIA Portal. */
int session_prepare(tool_ctx *c, unsigned flags);

th session_portal(void);
th session_project(void); /* current primary project (refreshed by session_prepare) */
long session_pid(void);
int session_launched(void);
int session_with_ui(void);

/* Attach to a running TIA Portal V21 (pid 0 = prefer one with a project open). */
int session_attach(tool_ctx *c, long pid);
/* Start a new TIA Portal instance. */
int session_launch(tool_ctx *c, int with_ui);
/* Attach to a running instance or launch one (used by open/create). */
int session_ensure_portal(tool_ctx *c, int with_ui);
/* Re-reads the open project. Returns its handle or 0. */
th session_refresh_project(void);
/* Drops the connection (TIA keeps running unless we launched it). */
void session_release(void);
/* Closes the TIA Portal process we are connected to. */
int session_close_tia(tool_ctx *c);
/* Fails when any PLC of the project is online. */
int session_check_offline(tool_ctx *c);

/* After each tool call: appends collected TIA notifications and detects a lost connection. */
void session_finish_call(tool_ctx *c);
void session_shutdown(void);

#endif
