/* Local copy of the TIA Portal hardware catalog (searchable without TIA) and
   the device profiles (CPUs / head modules) seen by the server. Both live in
   %LOCALAPPDATA%\tiaComandante and replace TiaCommander's SQLite tables. */
#ifndef TC_DEVCATALOG_H
#define TC_DEVCATALOG_H

#include "util/strbuf.h"

#include <stddef.h>

/* Catalog file: one entry per line, tab-separated
   ArticleNumber, Version, TypeName, TypeIdentifier, CatalogPath, Description;
   lines starting with '#' are comments. */
void dc_catalog_path(char *out, size_t cap);
/* Removes characters that would break the TSV format (tabs, line breaks). */
void dc_clean_field(char *s);
/* Writes the catalog atomically. */
int dc_catalog_write(const char *data, size_t len);
/* Case-insensitive search: every whitespace-separated term must occur in the
   entry. Appends up to limit result lines to out. Returns the number of
   matches, -1 if there is no local catalog. *info receives the header comment. */
int dc_catalog_search(const char *filter, int limit, strbuf *out, char *info, size_t info_cap);
int dc_catalog_delete(void);

/* Records a CPU / head module seen in a project (kept once per article number
   and firmware). */
void dc_profile_seen(const char *kind, const char *type_name, const char *order, const char *firmware);
/* One line per profile; returns the count. */
int dc_profiles_list(strbuf *out);

/* Prints one catalog line (shared by the live and the local search). */
void dc_format_entry(strbuf *out, const char *article, const char *version, const char *type_name,
                     const char *type_id, const char *path);

#endif
