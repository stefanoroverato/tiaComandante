/* Interface member editing on SimaticML trees (DB, UDT and block interfaces)
   and the export -> edit -> import round trip. */
#ifndef TC_MEMBERS_H
#define TC_MEMBERS_H

#include "mcp/tool.h"
#include "mxml.h"
#include "tia/tia_dyn.h"

/* Language used for new comments: the project's editing language (e.g. "en-US"). */
const char *mb_editing_language(th project);

/* Quotes PLC data type / FB names used as data types ("MyUdt" -> "\"MyUdt\""),
   also inside "Array[..] of X". Result in out. */
void mb_normalize_datatype(th plc_software, const char *in, char *out, size_t cap);

/* Creates <Member Name Datatype> with optional start value and comment (NULL/"" = none). */
mxml_node_t *mb_new_member(const char *name, const char *datatype, const char *start_value, const char *comment,
                           const char *lang);
void mb_set_comment(mxml_node_t *member, const char *lang, const char *text); /* "" removes */
void mb_set_start_value(mxml_node_t *member, const char *value);              /* "" removes */
/* Sets the start value of one array element ("Subelement Path=..."). */
void mb_set_element_start_value(mxml_node_t *member, const char *index, const char *value);

/* Finds the Section element by name (case-insensitive). */
mxml_node_t *mb_section(mxml_node_t *sections, const char *name);
/* Returns the element that holds the children of a member path's parent:
   for "a.b" the Member "a" (must be a Struct), for "b" the section. */
mxml_node_t *mb_parent_for(tool_ctx *c, mxml_node_t *sections, const char *section, const char *path, const char **leaf);
/* Lists the members per section as text (for "not found" hints). */
void mb_describe_members(mxml_node_t *sections, char *out, size_t cap);

/* Round trip: re-imports an edited SimaticML tree into the composition that owns obj
   (obj.Parent.<collection>.Import with ImportOptions.Override). Returns the new object or 0. */
th mb_reimport(tool_ctx *c, th obj, const char *collection, mxml_node_t *top);
/* Imports a SimaticML file into group.<collection>. Returns the first imported object or 0. */
th mb_import_file(tool_ctx *c, th group, const char *collection, const char *path, int override);


/* ---- editing session on an exported object -------------------------------------- */
typedef struct ed_doc {
    mxml_node_t *top;
    mxml_node_t *obj;      /* SW.Blocks.* / SW.Types.PlcStruct element */
    mxml_node_t *sections; /* Interface/Sections (may be NULL) */
    th item;
    const char *collection; /* "Blocks" or "Types": composition used for the re-import */
    char culture[32];
} ed_doc;

int ed_open(tool_ctx *c, th project, th item, const char *collection, ed_doc *d);
/* Re-imports the edited document. Returns the new object handle or 0. Frees the tree. */
th ed_commit(tool_ctx *c, ed_doc *d);
void ed_close(ed_doc *d);

/* Finds a member by dotted path (section optional); fails with the member list. */
mxml_node_t *ed_member(tool_ctx *c, ed_doc *d, const char *section, const char *path, const char **found_section);
int ed_add_member(tool_ctx *c, ed_doc *d, const char *section, const char *path, const char *datatype,
                  const char *start, const char *comment);
/* NULL arguments leave a property unchanged; "" removes start value / comment. */
int ed_update_member(tool_ctx *c, ed_doc *d, mxml_node_t *member, const char *new_name, const char *new_type,
                     const char *new_start, const char *new_comment);

/* Imports a new SimaticML tree into group.<collection>. Returns the object or 0. */
th mb_import_tree(tool_ctx *c, th group, const char *collection, mxml_node_t *top, int override);
/* Compiles an SCL/DB/UDT source text through a temporary external source into
   target_group (a user folder, or 0 for the container root). Returns the first
   generated object or 0. ext: ".scl", ".db" or ".udt". */
th mb_generate_from_source(tool_ctx *c, th plc_software, const char *name, const char *ext, const char *text,
                           th target_group);
/* Multilingual texts (Comment, Title) of an object for one culture. */
int mb_ml_set(th obj, const char *prop, const char *culture, const char *text);
char *mb_ml_get(th obj, const char *prop, const char *culture); /* malloc'ed, NULL if none */

/* Moves an object to another folder of the same container: export -> delete ->
   import into target_group; on import failure the object is restored in its
   original folder. Returns the new handle or 0. */
th mb_move(tool_ctx *c, th item, const char *collection, th target_group);
#endif
