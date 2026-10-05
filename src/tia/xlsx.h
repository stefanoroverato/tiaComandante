/* Minimal XLSX (Office Open XML spreadsheet) reader and writer.

   The zip container is handled by .NET (System.IO.Compression, part of the
   .NET Framework) through the bridge, the XML parts by Mini-XML: no extra
   dependency. The writer produces the layout of TIA Portal's own exports
   (shared strings, styles, custom properties), which its importers accept -
   a simpler file (inline strings, missing cells) can crash the TIA importer.

   A book is a cJSON array of sheets: [{"name": "Sheet", "rows": [["A1", "B1"], ...]}].
   Every row should have one string per column (empty strings are written too). */
#ifndef TC_XLSX_H
#define TC_XLSX_H

#include "cJSON.h"

#include <stddef.h>

/* file_content: value of the custom property FileContent (e.g. "Alarm text
   lists", as TIA writes it), or NULL. */
int xlsx_write(const char *path, const cJSON *book, const char *file_content, char *err, size_t errcap);

/* Reads every sheet: [{"name": ..., "rows": [[...], ...]}] (caller deletes).
   Cells are strings; missing cells are "". NULL on failure. */
cJSON *xlsx_read(const char *path, char *err, size_t errcap);

/* Extracts a zip archive into dir (created; existing files make it fail). */
int zip_extract(const char *zip, const char *dir, char *err, size_t errcap);

/* Rows of a sheet by name (case-insensitive), or NULL. */
const cJSON *xlsx_sheet_rows(const cJSON *book, const char *name);

/* Helpers to build books. */
cJSON *xlsx_add_sheet(cJSON *book, const char *name); /* returns the rows array */
cJSON *xlsx_add_row(cJSON *rows);                     /* returns the new row */
void xlsx_add_cell(cJSON *row, const char *text);
const char *xlsx_cell(const cJSON *row, int col);     /* "" when missing */

#endif
