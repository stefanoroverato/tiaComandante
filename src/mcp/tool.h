/* Tool/action model shared by the MCP server, the CLI and the tool modules.
 *
 * Every tool is a TiaCommander-style "meta tool": one MCP tool with an
 * "action" parameter selecting an action_def. Descriptions and the action
 * enum in the input schema are generated from these tables.
 */
#ifndef TC_TOOL_H
#define TC_TOOL_H

#include "cJSON.h"
#include "util/strbuf.h"

typedef struct tool_ctx tool_ctx;
typedef int (*action_fn)(tool_ctx *c);

/* Action flags (checked before the handler runs). */
enum {
    AF_BRIDGE = 1 << 0,      /* needs the .NET bridge */
    AF_PORTAL = 1 << 1,      /* needs an attached TIA Portal */
    AF_PROJECT = 1 << 2,     /* needs an open project */
    AF_WRITES = 1 << 3,      /* modifies project, PLC or files: refused in read-only mode */
    AF_DESTRUCTIVE = 1 << 4, /* documented as destructive (confirm gate inside the handler) */
    AF_OFFLINE = 1 << 5,     /* refused while a device is online */
};

typedef struct action_def {
    const char *name;
    const char *params;  /* human signature, e.g. "deviceName, blockName; optional outputPath" */
    const char *summary;
    action_fn fn;
    unsigned flags;
} action_def;

/* Tool annotations (MCP hints). */
enum { TH_READONLY = 1, TH_DESTRUCTIVE = 2, TH_IDEMPOTENT = 4 };

typedef struct tool_def {
    const char *name;
    const char *title;
    const char *summary;
    const char *properties; /* JSON object text with the parameter schemas (without "action") */
    const action_def *actions;
    int nactions;
    action_fn direct;       /* tools without actions */
    unsigned direct_flags;
    unsigned hints;
} tool_def;

struct tool_ctx {
    const tool_def *tool;
    const action_def *action;
    const cJSON *args;
    strbuf out;
    int is_error;
    const cJSON *progress_token;
    void (*progress_fn)(tool_ctx *c, double done, double total, const char *msg);
    volatile long *cancel;
    cJSON *owned; /* values parsed from stringified arguments, freed with the context */
    char error_context[256];
};

void ctx_init(tool_ctx *c, const tool_def *tool, const cJSON *args);
void ctx_free(tool_ctx *c);

/* ---- arguments (lenient with clients that stringify values) ---- */
const cJSON *arg_get(tool_ctx *c, const char *name);
int arg_has(tool_ctx *c, const char *name);
const char *arg_s(tool_ctx *c, const char *name);   /* NULL if absent or not a string/number */
const char *arg_req(tool_ctx *c, const char *name); /* fails the call if absent/empty */
long long arg_i(tool_ctx *c, const char *name, long long def);
double arg_d(tool_ctx *c, const char *name, double def);
int arg_b(tool_ctx *c, const char *name, int def);
const cJSON *arg_arr(tool_ctx *c, const char *name); /* array or NULL; accepts a JSON-encoded string */
const cJSON *arg_obj(tool_ctx *c, const char *name); /* object or NULL; accepts a JSON-encoded string */

/* ---- output ---- */
void out(tool_ctx *c, const char *fmt, ...);
void out_raw(tool_ctx *c, const char *text);
int fail(tool_ctx *c, const char *fmt, ...);
/* Fails with "<what>: <bridge/Openness error>". */
int fail_td(tool_ctx *c, const char *what);
void progress(tool_ctx *c, double done, double total, const char *msg);
int cancelled(tool_ctx *c);

/* Confirmation gate: returns 1 when arg "confirm" contains phrase (case-insensitive). */
int confirmed(tool_ctx *c, const char *phrase);

#endif
