#ifndef TC_TOOLS_H
#define TC_TOOLS_H

#include "mcp/tool.h"
#include "tia/tia_sw.h"
#include "util/fmt.h"

extern const tool_def tool_get_info;
extern const tool_def tool_session;
extern const tool_def tool_admin;
extern const tool_def tool_blocks_read;
extern const tool_def tool_blocks_write;
extern const tool_def tool_xref;
extern const tool_def tool_folders;
extern const tool_def tool_db;
extern const tool_def tool_udt;
extern const tool_def tool_tag;
extern const tool_def tool_watch;
extern const tool_def tool_diagnostics;
extern const tool_def tool_download_upload;
extern const tool_def tool_dev;

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Shared helpers (tools_common.c). */
/* Renders an enumeration item attribute as text (caller frees). */
char *item_text(const cJSON *item, const char *attr);
/* Short type name of a wire value ("Siemens.Engineering.SW.Blocks.FB" -> "FB"). */
const char *short_type(const cJSON *v);
/* Attributes read for items of a container, and the uniform item line
   "Folder/Name  [type=..., key=value, ...]". */
const char *item_attrs(sw_container cont);
void print_item(tool_ctx *c, sw_container cont, const cJSON *item, const char *folder);
/* Compiles obj (ICompilable) and reports state and messages (errors only if requested). */
int compile_object(tool_ctx *c, th obj, int errors_only);

#endif
