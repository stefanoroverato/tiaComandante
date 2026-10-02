#ifndef TC_REGISTRY_H
#define TC_REGISTRY_H

#include "mcp/tool.h"

int registry_count(void);
const tool_def *registry_at(int i);
const tool_def *registry_find(const char *name);
int registry_action_total(void);

/* Builds the MCP tools/list "tools" array. NULL if a schema is malformed
   (detail in err). */
cJSON *registry_tools_json(char *err, size_t errlen);

/* Runs one tool call: resolves the action, checks flags, opens a handle
   scope, records statistics. Output and error state end up in c. Returns 0
   if the tool exists (even when the call failed), -1 for an unknown tool. */
int registry_execute(tool_ctx *c, const char *tool_name);

#endif
