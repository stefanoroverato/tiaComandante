#ifndef TC_SERVER_H
#define TC_SERVER_H

/* Runs the MCP server on stdin/stdout until stdin closes. Returns the exit code. */
int mcp_serve(void);

/* Runs a single tool call (CLI --call). Prints the result to stdout. */
int mcp_call_once(const char *tool, const char *json_args);

#endif
