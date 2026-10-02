/* PLC software navigation: block/type/tag-table/watch-table trees, folder
   paths ("Motors/Drives"), name resolution and XML export. */
#ifndef TC_TIA_SW_H
#define TC_TIA_SW_H

#include "mcp/tool.h"
#include "tia/tia_dyn.h"
#include "tia/tia_nav.h"

#include "mxml.h"

/* Software containers (TiaCommander "container" names). */
typedef enum sw_container {
    SWC_BLOCKS = 0,       /* program_blocks: BlockGroup / Blocks */
    SWC_TAG_TABLES,       /* tag_tables: TagTableGroup / TagTables */
    SWC_TYPES,            /* udts: TypeGroup / Types */
    SWC_WATCH_TABLES,     /* watch_tables: WatchAndForceTableGroup / WatchTables + ForceTables */
    SWC_EXTERNAL_SOURCES, /* external_sources: ExternalSourceGroup / ExternalSources */
    SWC_TECH_OBJECTS,     /* technology_objects: TechnologicalObjectGroup / TechnologicalObjects */
    SWC_COUNT
} sw_container;

const char *sw_container_name(sw_container c);
int sw_container_parse(const char *name, sw_container *out);
th sw_container_root(th plc_software, sw_container c);

/* Item callback: item is the enumeration entry ($h, $t, a{attrs});
   folder is the folder path relative to the container root ("" = root). */
typedef int (*sw_item_fn)(void *ctx, const cJSON *item, const char *folder);
/* Folder callback (called before the folder's items). */
typedef int (*sw_folder_fn)(void *ctx, th group, const char *folder, int depth);

/* Walks a container recursively. attrs_csv: attributes read for every item. */
int sw_walk(th plc_software, sw_container c, const char *attrs_csv, sw_item_fn item_fn, sw_folder_fn folder_fn,
            void *ctx);
/* Walks from an arbitrary group. */
int sw_walk_group(th group, sw_container c, const char *attrs_csv, const char *folder, int depth, sw_item_fn item_fn,
                  sw_folder_fn folder_fn, void *ctx);

typedef struct sw_found {
    th item;
    char name[256];
    char folder[512];
    char type[64]; /* short type name: FB, FC, OB, GlobalDB, InstanceDB, PlcStruct, PlcTagTable, ... */
} sw_found;

/* Finds an item by name ("Name" or "Folder/Sub/Name", case-insensitive).
   On failure reports similar names. */
int sw_find(tool_ctx *c, th plc_software, sw_container cont, const char *name, sw_found *out);

/* Resolves a folder path; with create, missing folders are created when
   create_parents is set (or when only the last level is missing and
   create_last is set). Returns the group handle or 0 (call failed). */
th sw_folder(tool_ctx *c, th plc_software, sw_container cont, const char *path, int create, int create_parents);

/* Exports an object to a temporary SimaticML file (ExportOptions.WithDefaults). */
int sw_export_xml(tool_ctx *c, th obj, char *path_out, size_t cap);

/* Resolves deviceName to the PLC software (fails the call otherwise). */
int sw_plc(tool_ctx *c, nav_plc *plc);

/* Delivers file content to the caller: to outputPath (file, or folder ->
   folder\default_name), inline (small content) or through the export store.
   force_inline: always inline regardless of size. */
int sw_deliver(tool_ctx *c, const char *src_file, const char *default_name, const char *content_type,
               const char *output_path, int return_inline);

/* Exports obj and returns the parsed SimaticML tree (caller mxmlDelete); NULL on failure. */
mxml_node_t *sw_export_tree(tool_ctx *c, th obj);
#endif
