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
/* Opens a project file in the connected TIA Portal. Projects protected by user
   management (UMAC) are opened with credentials from the Windows Credential
   Manager (admin action=set_credential kind=umac). Returns the project or 0. */
th session_open_project(tool_ctx *c, const char *path, int upgrade);
/* Re-reads the open project. Returns its handle or 0. */
th session_refresh_project(void);
/* Drops the connection (TIA keeps running unless we launched it). */
void session_release(void);
/* Closes the TIA Portal process we are connected to. */
int session_close_tia(tool_ctx *c);
/* Fails when any PLC of the project is online. */
int session_check_offline(tool_ctx *c);

/* Guard around one project-modifying call (AF_PROJECT | AF_WRITES): an
   ExclusiveAccess (TIA shows a modal "tiaComandante: tool.action" dialog with a
   Cancel button) and, unless AF_NO_TX, a transaction on the project: one undo
   step in TIA Portal, and every change of the call is rolled back when it fails
   or is cancelled. Config: exclusiveAccess, transactions. begin returns -1 (and
   fails the call) when TIA Portal refuses exclusive access. */
typedef struct session_guard {
    th access;
    th tx;
    int splits;   /* transactions committed early, around a compile */
    int poisoned; /* TIA refused a commit before a compile (an exception happened inside the transaction) */
    char label[160];
} session_guard;
int session_guard_begin(tool_ctx *c, unsigned flags, session_guard *g);
/* Returns 1 when the call must be repeated without a transaction: TIA Portal
   does not commit a transaction once an exception was thrown inside it, even
   when the action recovered from it (e.g. import, compile, import again); the
   changes were rolled back, so a rerun starts from the original state. */
int session_guard_end(tool_ctx *c, session_guard *g);

/* ICompilable.Compile(). TIA does not permit compiling inside a transaction:
   in a guarded call the transaction is committed before the compile and a new
   one is opened after it. Returns the CompilerResult, or 0 (td_err set). */
th session_compile(th compilable);

/* Around each tool call. The confirmations policy answers only while a call runs: TIA Portal also
   sends the confirmations of its own user interface to the subscribed client. */
void session_call_begin(void);
void session_call_end(void);

/* After each tool call: appends collected TIA notifications and detects a lost connection. */
void session_finish_call(tool_ctx *c);
void session_shutdown(void);

#endif
