#include "xlsx.h"

#include "tia/simaticml.h"
#include "tia/tia_dyn.h"
#include "util/fs.h"
#include "util/strbuf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET_ZIPFILE "System.IO.Compression.ZipFile, System.IO.Compression.FileSystem, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089"
#define NET_ZIPEXT "System.IO.Compression.ZipFileExtensions, System.IO.Compression.FileSystem, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089"
#define NET_ZIPMODE "System.IO.Compression.ZipArchiveMode, System.IO.Compression, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089"

#define NS_MAIN "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
#define NS_REL "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
#define NS_PKG_REL "http://schemas.openxmlformats.org/package/2006/relationships"

/* ---- building books ------------------------------------------------------------------------- */

cJSON *xlsx_add_sheet(cJSON *book, const char *name)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "name", name);
    cJSON *rows = cJSON_AddArrayToObject(s, "rows");
    cJSON_AddItemToArray(book, s);
    return rows;
}

cJSON *xlsx_add_row(cJSON *rows)
{
    cJSON *r = cJSON_CreateArray();
    cJSON_AddItemToArray(rows, r);
    return r;
}

void xlsx_add_cell(cJSON *row, const char *text)
{
    cJSON_AddItemToArray(row, cJSON_CreateString(text ? text : ""));
}

const char *xlsx_cell(const cJSON *row, int col)
{
    const char *s = cJSON_GetStringValue(cJSON_GetArrayItem(row, col));
    return s ? s : "";
}

const cJSON *xlsx_sheet_rows(const cJSON *book, const char *name)
{
    const cJSON *s;
    cJSON_ArrayForEach(s, book)
    {
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s, "name"));
        if (n && _stricmp(n, name) == 0)
            return cJSON_GetObjectItemCaseSensitive(s, "rows");
    }
    return NULL;
}

/* ---- writer ------------------------------------------------------------------------------------ */

static void xml_escape(strbuf *sb, const char *s)
{
    for (; s && *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '&')
            sb_append(sb, "&amp;");
        else if (ch == '<')
            sb_append(sb, "&lt;");
        else if (ch == '>')
            sb_append(sb, "&gt;");
        else if (ch == '"')
            sb_append(sb, "&quot;");
        else if (ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r')
            continue; /* not allowed in XML 1.0 */
        else
            sb_appendc(sb, (char)ch);
    }
}

static void col_name(int col, char *out)
{
    char tmp[8];
    int n = 0;
    col++;
    while (col > 0 && n < 7) {
        int r = (col - 1) % 26;
        tmp[n++] = (char)('A' + r);
        col = (col - 1) / 26;
    }
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

typedef struct sst {
    cJSON *index; /* string -> position */
    strbuf xml;
    int count;
} sst;

static int sst_id(sst *t, const char *s)
{
    const cJSON *hit = cJSON_GetObjectItemCaseSensitive(t->index, s);
    if (hit)
        return (int)hit->valuedouble;
    cJSON_AddNumberToObject(t->index, s, t->count);
    sb_append(&t->xml, "<x:si><x:t xml:space=\"preserve\">");
    xml_escape(&t->xml, s);
    sb_append(&t->xml, "</x:t></x:si>");
    return t->count++;
}

static int write_part(const char *dir, const char *name, const char *text)
{
    char path[1024], parent[1024];
    fs_join(path, sizeof path, dir, name);
    for (char *p = path; *p; p++)
        if (*p == '/')
            *p = '\\';
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '\\');
    if (slash) {
        *slash = 0;
        fs_mkdirs(parent);
    }
    return fs_write_all(path, text, strlen(text));
}

static const char k_styles[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><x:styleSheet xmlns:x=\"" NS_MAIN "\"><x:numFmts count=\"0\" />"
    "<x:fonts count=\"1\"><x:font><x:sz val=\"11\" /><x:name val=\"Calibri\" /></x:font></x:fonts>"
    "<x:fills count=\"2\"><x:fill><x:patternFill patternType=\"none\" /></x:fill><x:fill><x:patternFill patternType=\"gray125\" /></x:fill></x:fills>"
    "<x:borders count=\"1\"><x:border><x:left /><x:right /><x:top /><x:bottom /><x:diagonal /></x:border></x:borders>"
    "<x:cellStyleXfs count=\"1\"><x:xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" /></x:cellStyleXfs>"
    "<x:cellXfs count=\"1\"><x:xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyAlignment=\"1\" /></x:cellXfs>"
    "<x:cellStyles count=\"1\"><x:cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\" /></x:cellStyles><x:dxfs count=\"0\" />"
    "<x:tableStyles count=\"0\" defaultTableStyle=\"TableStyleMedium9\" defaultPivotStyle=\"PivotStyleLight16\" /></x:styleSheet>";

/* Zips the parts written in dir into path, entry names with '/'. */
static int zip_parts(const char *path, const char *dir, const char *const *parts, int nparts, char *err, size_t errcap)
{
    fs_remove(path);
    cJSON *arc = td_static(NET_ZIPFILE, "Open", tda("se", path, NET_ZIPMODE, "Create"));
    th a = tdv_h(arc);
    cJSON_Delete(arc);
    if (!a) {
        snprintf(err, errcap, "cannot create %s: %s", path, td_err());
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < nparts && rc == 0; i++) {
        char file[1024];
        fs_join(file, sizeof file, dir, parts[i]);
        for (char *p = file; *p; p++)
            if (*p == '/')
                *p = '\\';
        cJSON *e = td_static(NET_ZIPEXT, "CreateEntryFromFile", tda("hss", a, file, parts[i]));
        if (!e) {
            snprintf(err, errcap, "zip entry %s: %s", parts[i], td_err());
            rc = -1;
        }
        cJSON_Delete(e);
    }
    td_call_v(a, "Dispose", NULL);
    td_release(a);
    return rc;
}

int xlsx_write(const char *path, const cJSON *book, const char *file_content, char *err, size_t errcap)
{
    char dir[1024];
    if (fs_temp_dir("xlsx", dir, sizeof dir) != 0) {
        snprintf(err, errcap, "cannot create a temporary folder");
        return -1;
    }
    int nsheets = cJSON_GetArraySize(book);
    sst t = { cJSON_CreateObject(), { 0 }, 0 };
    sb_init(&t.xml);
    strbuf sb;
    sb_init(&sb);
    char name[64];
    int rc = 0;
    /* sheets first: they fill the shared string table */
    for (int i = 0; i < nsheets && rc == 0; i++) {
        const cJSON *rows = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(book, i), "rows");
        sb_clear(&sb);
        sb_append(&sb, "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?><x:worksheet xmlns:x=\"" NS_MAIN "\"><x:sheetData>");
        int r = 0;
        const cJSON *row;
        cJSON_ArrayForEach(row, rows)
        {
            r++;
            sb_printf(&sb, "<x:row r=\"%d\">", r);
            int c = 0;
            const cJSON *cell;
            cJSON_ArrayForEach(cell, row)
            {
                char cn[8];
                col_name(c++, cn);
                const char *v = cJSON_GetStringValue(cell);
                sb_printf(&sb, "<x:c r=\"%s%d\" s=\"0\" t=\"s\"><x:v>%d</x:v></x:c>", cn, r, sst_id(&t, v ? v : ""));
            }
            sb_append(&sb, "</x:row>");
        }
        sb_append(&sb, "</x:sheetData></x:worksheet>");
        snprintf(name, sizeof name, "xl/worksheets/sheet%d.xml", i + 1);
        rc = write_part(dir, name, sb_str(&sb));
    }
    /* workbook, relationships, content types */
    sb_clear(&sb);
    sb_append(&sb, "<?xml version=\"1.0\" encoding=\"utf-8\"?><x:workbook xmlns:x=\"" NS_MAIN "\"><x:sheets>");
    for (int i = 0; i < nsheets; i++) {
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(book, i), "name"));
        sb_append(&sb, "<x:sheet name=\"");
        xml_escape(&sb, n ? n : "Sheet");
        sb_printf(&sb, "\" sheetId=\"%d\" r:id=\"S%d\" xmlns:r=\"" NS_REL "\" />", i + 1, i + 1);
    }
    sb_append(&sb, "</x:sheets></x:workbook>");
    if (rc == 0)
        rc = write_part(dir, "xl/workbook.xml", sb_str(&sb));
    sb_clear(&sb);
    sb_append(&sb, "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?><Relationships xmlns=\"" NS_PKG_REL "\">"
                   "<Relationship Type=\"" NS_REL "/styles\" Target=\"/xl/styles.xml\" Id=\"ST\" />");
    for (int i = 0; i < nsheets; i++)
        sb_printf(&sb, "<Relationship Type=\"" NS_REL "/worksheet\" Target=\"/xl/worksheets/sheet%d.xml\" Id=\"S%d\" />", i + 1, i + 1);
    sb_append(&sb, "<Relationship Type=\"" NS_REL "/sharedStrings\" Target=\"/xl/sharedStrings.xml\" Id=\"SS\" /></Relationships>");
    if (rc == 0)
        rc = write_part(dir, "xl/_rels/workbook.xml.rels", sb_str(&sb));
    sb_clear(&sb);
    sb_printf(&sb, "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?><x:sst uniqueCount=\"%d\" xmlns:x=\"" NS_MAIN "\">%s</x:sst>",
              t.count, sb_str(&t.xml));
    if (rc == 0)
        rc = write_part(dir, "xl/sharedStrings.xml", sb_str(&sb));
    if (rc == 0)
        rc = write_part(dir, "xl/styles.xml", k_styles);
    sb_clear(&sb);
    sb_append(&sb, "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                   "<Default Extension=\"xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\" />"
                   "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\" />"
                   "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\" />");
    if (file_content)
        sb_append(&sb, "<Override PartName=\"/docProps/custom.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.custom-properties+xml\" />");
    for (int i = 0; i < nsheets; i++)
        sb_printf(&sb, "<Override PartName=\"/xl/worksheets/sheet%d.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\" />", i + 1);
    sb_append(&sb, "<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\" /></Types>");
    if (rc == 0)
        rc = write_part(dir, "[Content_Types].xml", sb_str(&sb));
    sb_clear(&sb);
    sb_append(&sb, "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?><Relationships xmlns=\"" NS_PKG_REL "\">"
                   "<Relationship Type=\"" NS_REL "/officeDocument\" Target=\"/xl/workbook.xml\" Id=\"R1\" />");
    if (file_content)
        sb_append(&sb, "<Relationship Type=\"" NS_REL "/custom-properties\" Target=\"/docProps/custom.xml\" Id=\"R2\" />");
    sb_append(&sb, "</Relationships>");
    if (rc == 0)
        rc = write_part(dir, "_rels/.rels", sb_str(&sb));
    if (file_content && rc == 0) {
        sb_clear(&sb);
        sb_append(&sb, "<?xml version=\"1.0\" encoding=\"utf-8\"?><op:Properties xmlns:op=\"http://schemas.openxmlformats.org/officeDocument/2006/custom-properties\">"
                       "<op:property fmtid=\"{D5CDD505-2E9C-101B-9397-08002B2CF9AE}\" pid=\"2\" name=\"FileVersion\"><vt:lpwstr "
                       "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\">1</vt:lpwstr></op:property>"
                       "<op:property fmtid=\"{D5CDD505-2E9C-101B-9397-08002B2CF9AE}\" pid=\"3\" name=\"FileContent\"><vt:lpwstr "
                       "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\">");
        xml_escape(&sb, file_content);
        sb_append(&sb, "</vt:lpwstr></op:property></op:Properties>");
        rc = write_part(dir, "docProps/custom.xml", sb_str(&sb));
    }
    sb_free(&sb);
    sb_free(&t.xml);
    cJSON_Delete(t.index);
    if (rc != 0) {
        snprintf(err, errcap, "cannot write the workbook parts");
    } else {
        const char *parts[64];
        char names[64][64];
        int np = 0;
        parts[np++] = "[Content_Types].xml";
        parts[np++] = "_rels/.rels";
        if (file_content)
            parts[np++] = "docProps/custom.xml";
        parts[np++] = "xl/workbook.xml";
        parts[np++] = "xl/_rels/workbook.xml.rels";
        parts[np++] = "xl/styles.xml";
        parts[np++] = "xl/sharedStrings.xml";
        for (int i = 0; i < nsheets && np < 64; i++) {
            snprintf(names[i], sizeof names[i], "xl/worksheets/sheet%d.xml", i + 1);
            parts[np++] = names[i];
        }
        rc = zip_parts(path, dir, parts, np, err, errcap);
    }
    fs_remove_tree(dir);
    return rc;
}

/* ---- reader ------------------------------------------------------------------------------------ */

/* Element name without namespace prefix. */
static const char *local(mxml_node_t *n)
{
    const char *e = n && mxmlGetType(n) == MXML_TYPE_ELEMENT ? mxmlGetElement(n) : NULL;
    const char *colon = e ? strchr(e, ':') : NULL;
    return colon ? colon + 1 : e;
}

static mxml_node_t *child_l(mxml_node_t *n, const char *name)
{
    for (mxml_node_t *c = n ? mxmlGetFirstChild(n) : NULL; c; c = mxmlGetNextSibling(c)) {
        const char *l = local(c);
        if (l && strcmp(l, name) == 0)
            return c;
    }
    return NULL;
}

static mxml_node_t *next_l(mxml_node_t *n, const char *name)
{
    for (mxml_node_t *c = n ? mxmlGetNextSibling(n) : NULL; c; c = mxmlGetNextSibling(c)) {
        const char *l = local(c);
        if (l && strcmp(l, name) == 0)
            return c;
    }
    return NULL;
}

static mxml_node_t *find_l(mxml_node_t *n, const char *name)
{
    for (mxml_node_t *c = n ? mxmlGetFirstChild(n) : NULL; c; c = mxmlGetNextSibling(c)) {
        const char *l = local(c);
        if (l && strcmp(l, name) == 0)
            return c;
        mxml_node_t *d = find_l(c, name);
        if (d)
            return d;
    }
    return NULL;
}

/* Attribute by local name (r:id, id, ...). */
static const char *attr_l(mxml_node_t *n, const char *name)
{
    size_t count = mxmlElementGetAttrCount(n);
    for (size_t i = 0; i < count; i++) {
        const char *an = NULL;
        const char *v = mxmlElementGetAttrByIndex(n, i, &an);
        const char *colon = an ? strchr(an, ':') : NULL;
        const char *l = colon ? colon + 1 : an;
        if (l && strcmp(l, name) == 0)
            return v;
    }
    return NULL;
}

/* Concatenated text of all <t> below n (plain and rich text). */
static void collect_t(mxml_node_t *n, strbuf *sb)
{
    for (mxml_node_t *c = n ? mxmlGetFirstChild(n) : NULL; c; c = mxmlGetNextSibling(c)) {
        const char *l = local(c);
        if (!l)
            continue;
        if (strcmp(l, "t") == 0)
            sb_append(sb, sml_text(c));
        else if (strcmp(l, "rPh") != 0) /* phonetic runs are not part of the value */
            collect_t(c, sb);
    }
}

static mxml_node_t *load_part(const char *dir, const char *part)
{
    char path[1024];
    while (*part == '/')
        part++;
    fs_join(path, sizeof path, dir, part);
    for (char *p = path; *p; p++)
        if (*p == '/')
            *p = '\\';
    char err[128];
    return sml_load_file(path, err, sizeof err);
}

static int col_index(const char *ref)
{
    int c = 0;
    for (; ref && isalpha((unsigned char)*ref); ref++)
        c = c * 26 + (toupper((unsigned char)*ref) - 'A' + 1);
    return c - 1;
}

int zip_extract(const char *zip, const char *dir, char *err, size_t errcap)
{
    cJSON *ok = td_static(NET_ZIPFILE, "ExtractToDirectory", tda("ss", zip, dir));
    if (!ok) {
        snprintf(err, errcap, "cannot extract %s: %s", zip, td_err());
        return -1;
    }
    cJSON_Delete(ok);
    return 0;
}

cJSON *xlsx_read(const char *path, char *err, size_t errcap)
{
    char dir[1024];
    if (fs_temp_dir("xlsxr", dir, sizeof dir) != 0) {
        snprintf(err, errcap, "cannot create a temporary folder");
        return NULL;
    }
    fs_remove_tree(dir); /* ExtractToDirectory wants to create it */
    if (zip_extract(path, dir, err, errcap) != 0) {
        fs_remove_tree(dir);
        return NULL;
    }
    mxml_node_t *wb = load_part(dir, "xl/workbook.xml");
    mxml_node_t *rels = load_part(dir, "xl/_rels/workbook.xml.rels");
    mxml_node_t *sst = load_part(dir, "xl/sharedStrings.xml");
    cJSON *strings = cJSON_CreateArray();
    for (mxml_node_t *si = sst ? child_l(find_l(sst, "sst"), "si") : NULL; si; si = next_l(si, "si")) {
        strbuf sb;
        sb_init(&sb);
        collect_t(si, &sb);
        cJSON_AddItemToArray(strings, cJSON_CreateString(sb_str(&sb)));
        sb_free(&sb);
    }
    cJSON *book = NULL;
    mxml_node_t *sheets = wb ? find_l(wb, "sheets") : NULL;
    if (!sheets) {
        snprintf(err, errcap, "%s is not a valid workbook (no xl/workbook.xml)", path);
    } else {
        book = cJSON_CreateArray();
        for (mxml_node_t *s = child_l(sheets, "sheet"); s; s = next_l(s, "sheet")) {
            const char *name = attr_l(s, "name");
            const char *rid = attr_l(s, "id");
            const char *target = NULL;
            for (mxml_node_t *r = rels ? child_l(find_l(rels, "Relationships"), "Relationship") : NULL; r && rid;
                 r = next_l(r, "Relationship")) {
                const char *id = attr_l(r, "Id");
                if (id && strcmp(id, rid) == 0)
                    target = attr_l(r, "Target");
            }
            cJSON *rows = xlsx_add_sheet(book, name ? name : "?");
            if (!target)
                continue;
            char part[512];
            snprintf(part, sizeof part, "%s%s", target[0] == '/' ? "" : "xl/", target);
            mxml_node_t *ws = load_part(dir, part);
            mxml_node_t *data = ws ? find_l(ws, "sheetData") : NULL;
            int expected = 1;
            for (mxml_node_t *row = child_l(data, "row"); row; row = next_l(row, "row")) {
                const char *rr = attr_l(row, "r");
                int rn = rr ? atoi(rr) : expected;
                while (expected < rn) { /* keep row numbers aligned */
                    xlsx_add_row(rows);
                    expected++;
                }
                cJSON *out = xlsx_add_row(rows);
                expected = rn + 1;
                int next_col = 0;
                for (mxml_node_t *c = child_l(row, "c"); c; c = next_l(c, "c")) {
                    const char *ref = attr_l(c, "r");
                    int ci = ref ? col_index(ref) : next_col;
                    while (next_col < ci) {
                        xlsx_add_cell(out, "");
                        next_col++;
                    }
                    const char *t = attr_l(c, "t");
                    strbuf v;
                    sb_init(&v);
                    if (t && strcmp(t, "inlineStr") == 0) {
                        collect_t(child_l(c, "is"), &v);
                    } else {
                        const char *raw = sml_text(child_l(c, "v"));
                        if (t && strcmp(t, "s") == 0)
                            sb_append(&v, cJSON_GetStringValue(cJSON_GetArrayItem(strings, atoi(raw))) ? cJSON_GetStringValue(cJSON_GetArrayItem(strings, atoi(raw))) : "");
                        else
                            sb_append(&v, raw);
                    }
                    xlsx_add_cell(out, sb_str(&v));
                    sb_free(&v);
                    next_col = ci + 1;
                }
            }
            if (ws)
                mxmlDelete(ws);
        }
    }
    if (wb)
        mxmlDelete(wb);
    if (rels)
        mxmlDelete(rels);
    if (sst)
        mxmlDelete(sst);
    cJSON_Delete(strings);
    fs_remove_tree(dir);
    return book;
}
