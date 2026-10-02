/* SimaticML (TIA Portal XML export) helpers on top of Mini-XML. Element and
   attribute names are matched without namespace handling: SimaticML uses
   default namespaces only. */
#ifndef TC_SIMATICML_H
#define TC_SIMATICML_H

#include "mxml.h"
#include "util/strbuf.h"

#include <stddef.h>

/* Loads a document (UTF-8, optional BOM). Returns the tree top or NULL. */
mxml_node_t *sml_load_file(const char *path, char *err, size_t errlen);
mxml_node_t *sml_load_string(const char *text, char *err, size_t errlen);
/* Serialises the whole tree. Returns 0 on success. */
int sml_save_file(mxml_node_t *top, const char *path);
char *sml_save_string(mxml_node_t *top); /* malloc'ed */

mxml_node_t *sml_child(mxml_node_t *n, const char *name);     /* first child element (name NULL = any) */
mxml_node_t *sml_next(mxml_node_t *n, const char *name);      /* next sibling element */
mxml_node_t *sml_find(mxml_node_t *top, const char *name);    /* first descendant element */
mxml_node_t *sml_path(mxml_node_t *n, const char *path);      /* "A/B/C" through child elements */
const char *sml_text(mxml_node_t *n);                         /* text content of a leaf element ("" if none) */
const char *sml_child_text(mxml_node_t *n, const char *name); /* NULL if the child does not exist */
const char *sml_attr(mxml_node_t *n, const char *name);       /* NULL if missing */
void sml_set_text(mxml_node_t *n, const char *text);          /* replaces the content of a leaf element */
mxml_node_t *sml_ensure_child(mxml_node_t *n, const char *name, int before_first);

/* The exported engineering object: Document/<SW.Blocks.FB|SW.Types.PlcStruct|...>. */
mxml_node_t *sml_object(mxml_node_t *top);
mxml_node_t *sml_attribute_list(mxml_node_t *obj);
mxml_node_t *sml_sections(mxml_node_t *obj); /* AttributeList/Interface/Sections */

/* Multilingual texts of an object, e.g. composition "Title" or "Comment".
   lang NULL = first non-empty. */
const char *sml_ml_text(mxml_node_t *obj, const char *composition, const char *lang);
/* Member comment (Comment/MultiLanguageText), lang NULL = first non-empty. */
const char *sml_member_comment(mxml_node_t *member, const char *lang);

/* Interface members. path is the dotted member path, section the section name. */
typedef int (*sml_member_fn)(void *ctx, mxml_node_t *member, const char *section, const char *path, int depth);
int sml_walk_members(mxml_node_t *sections, int max_depth, sml_member_fn fn, void *ctx);
/* Finds a member by dotted path (case-insensitive), optionally in one section. */
mxml_node_t *sml_find_member(mxml_node_t *sections, const char *section, const char *path, const char **found_section);
/* Start value of a member (StartValue element), NULL if none. */
const char *sml_start_value(mxml_node_t *member);

/* Networks (SW.Blocks.CompileUnit objects) in document order. */
int sml_compile_unit_count(mxml_node_t *obj);
mxml_node_t *sml_compile_unit(mxml_node_t *obj, int index);
/* Renders an SCL StructuredText network source as text. */
void sml_render_scl(mxml_node_t *structured_text, strbuf *out);
/* Renders one Access element (operand) as SCL text. */
void sml_render_access(mxml_node_t *access, strbuf *out);

#endif
