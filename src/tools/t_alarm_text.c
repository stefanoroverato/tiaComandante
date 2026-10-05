/* alarm_text: PLC alarm text lists and their entries, alarm instance texts and
   alarm classes.

   Openness V21 has no API for the entries of a text list: they exist only in
   TIA Portal's Excel export/import (PlcAlarmTextListProvider). The entry
   actions export the list, change the rows (tia/xlsx) and import it again with
   ImportOptions.Override, which replaces the entries of the lists in the file. */
#include "tools.h"

#include "app/config.h"
#include "tia/session.h"
#include "tia/simaticml.h"
#include "tia/tia_dyn.h"
#include "tia/tia_nav.h"
#include "tia/tia_sw.h"
#include "tia/xlsx.h"
#include "util/fs.h"
#include "util/strbuf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T_TL_PROVIDER "Siemens.Engineering.SW.Alarm.PlcAlarmTextListProvider"
#define T_TEXT_PROVIDER "Siemens.Engineering.SW.Alarm.PlcAlarmTextProvider"
#define T_CLASS_PROVIDER "Siemens.Engineering.SW.Alarm.AlarmClassDataProvider"
#define XLSX_TYPE "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
#define TL_CONTENT "Alarm text lists"

/* ---- helpers ---------------------------------------------------------------------------------- */

static void editing_culture(char *out, size_t cap)
{
    th ls = td_get_h(session_project(), "LanguageSettings");
    th lang = ls ? td_get_h(ls, "EditingLanguage") : 0;
    cJSON *cu = lang ? td_get(lang, "Culture") : NULL;
    const char *s = tdv_s(cu);
    snprintf(out, cap, "%s", s && *s ? s : "en-US");
    cJSON_Delete(cu);
    td_clear_err();
}

static const char *language(tool_ctx *c, char *buf, size_t cap)
{
    const char *l = arg_s(c, "language");
    if (l && *l)
        return l;
    editing_culture(buf, cap);
    return buf;
}

static char *ml_text(th ml)
{
    cJSON *items = ml ? td_enum(td_get_h(ml, "Items"), "Text", -1) : NULL;
    char *res = NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        const char *t = tdi_s(it, "Text");
        if (t && *t) {
            res = _strdup(t);
            break;
        }
    }
    cJSON_Delete(items);
    td_clear_err();
    return res;
}

/* Sets the text of one language of a MultilingualText. */
static int ml_set(th ml, const char *culture, const char *text)
{
    cJSON *items = ml ? td_enum(td_get_h(ml, "Items"), "Language", -1) : NULL;
    int rc = -1;
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        cJSON *cu = td_get(tdv_h(tdi_a(it, "Language")), "Culture");
        const char *s = tdv_s(cu);
        int hit = s && _stricmp(s, culture) == 0;
        cJSON_Delete(cu);
        if (hit) {
            rc = td_set(tdv_h(it), "Text", cJSON_CreateString(text));
            break;
        }
    }
    cJSON_Delete(items);
    return rc;
}

typedef struct tl_ref {
    th item;
    char name[256];
    char id[32];
    char range[16];
    int system;
} tl_ref;

static int find_textlist(tool_ctx *c, const nav_plc *p, const char *name, tl_ref *out, int quiet)
{
    memset(out, 0, sizeof *out);
    th group = td_get_h(p->software, "PlcAlarmTextlistGroup");
    strbuf names;
    sb_init(&names);
    for (int sys = 0; sys < 2 && !out->item; sys++) {
        cJSON *lists = td_enum(td_get_h(group, sys ? "PlcAlarmSystemTextlists" : "PlcAlarmUserTextlists"), "Name,ID,ListRange", -1);
        const cJSON *it;
        cJSON_ArrayForEach(it, lists)
        {
            const char *n = tdi_s(it, "Name");
            if (!sys && names.len < 2000)
                sb_printf(&names, "%s%s", names.len ? ", " : "", n ? n : "?");
            if (n && _stricmp(n, name) == 0) {
                out->item = tdv_h(it);
                snprintf(out->name, sizeof out->name, "%s", n);
                cJSON *id = cJSON_Duplicate(tdi_a(it, "ID"), 1);
                char *t = tdv_text(id);
                snprintf(out->id, sizeof out->id, "%s", t ? t : "");
                free(t);
                cJSON_Delete(id);
                snprintf(out->range, sizeof out->range, "%s", tdi_s(it, "ListRange") ? tdi_s(it, "ListRange") : "?");
                out->system = sys;
                break;
            }
        }
        cJSON_Delete(lists);
    }
    td_clear_err();
    if (!out->item && !quiet)
        fail(c, "text list '%s' not found in %s. User text lists: %s", name, p->device_name, names.len ? sb_str(&names) : "(none)");
    sb_free(&names);
    return out->item ? 0 : -1;
}

/* Messages of a TIA import/export log (<LogEntry type=...><Message>...). */
static void log_messages(const char *path, strbuf *sb, int skip_info)
{
    char err[128];
    mxml_node_t *top = path && *path ? sml_load_file(path, err, sizeof err) : NULL;
    mxml_node_t *log = top ? sml_find(top, "LogFile") : NULL;
    for (mxml_node_t *e = sml_child(log, "LogEntry"); e; e = sml_next(e, "LogEntry")) {
        const char *type = sml_attr(e, "type");
        if (skip_info && type && strcmp(type, "Information") == 0)
            continue;
        const char *m = sml_child_text(e, "Message");
        sb_printf(sb, "  %s: %s\n", type ? type : "?", m ? m : "");
    }
    if (top)
        mxmlDelete(top);
}

/* Checks a TextListXlsxResult / PlcAlarmTextXlsxResult; prints the log. */
static int xlsx_result(tool_ctx *c, cJSON *res, const char *what)
{
    th r = tdv_h(res);
    char *state = r ? td_get_s(r, "State") : NULL;
    cJSON *lp = r ? td_get(r, "LogFilePath") : NULL;
    strbuf msgs;
    sb_init(&msgs);
    log_messages(tdv_s(lp), &msgs, 0);
    cJSON_Delete(lp);
    td_clear_err();
    int ok = state && strcmp(state, "OK") == 0;
    int warn = state && strcmp(state, "Warning") == 0;
    if (ok || warn) {
        if (msgs.len)
            out(c, "%s%s", warn ? "TIA Portal reported warnings:\n" : "", sb_str(&msgs));
    } else {
        fail(c, "%s: %s\n%s", what, state ? state : "failed", sb_str(&msgs));
    }
    free(state);
    sb_free(&msgs);
    return ok || warn ? 0 : -1;
}

/* Exports one text list (or all with name NULL) and reads the workbook. */
static cJSON *export_book(tool_ctx *c, const nav_plc *p, const char *name)
{
    th tlp = td_service(p->software, T_TL_PROVIDER);
    if (!tlp) {
        fail_td(c, "the text list service is not available for this PLC");
        return NULL;
    }
    char path[TC_PATH_MAX];
    if (fs_temp_path("textlists", ".xlsx", path, sizeof path) != 0) {
        fail(c, "cannot create a temporary file");
        return NULL;
    }
    fs_remove(path);
    cJSON *names = cJSON_CreateArray();
    if (name)
        cJSON_AddItemToArray(names, cJSON_CreateString(name));
    cJSON *res = name ? td_call(tlp, "ExportToXlsx", tda("fjn", path, names)) : (cJSON_Delete(names), td_call(tlp, "ExportToXlsx", tda("f", path)));
    if (!res) {
        fail_td(c, "exporting the text lists failed");
        fs_remove(path);
        return NULL;
    }
    char err[256];
    cJSON *book = xlsx_result(c, res, "exporting the text lists failed") == 0 ? xlsx_read(path, err, sizeof err) : NULL;
    cJSON_Delete(res);
    if (!book && !c->is_error)
        fail(c, "%s", err);
    fs_remove(path);
    return book;
}

static int import_book(tool_ctx *c, const nav_plc *p, const cJSON *book, int override, const char *what)
{
    char path[TC_PATH_MAX], err[256];
    if (fs_temp_path("textlists", ".xlsx", path, sizeof path) != 0)
        return fail(c, "cannot create a temporary file");
    if (xlsx_write(path, book, TL_CONTENT, err, sizeof err) != 0)
        return fail(c, "%s", err);
    th tlp = td_service(p->software, T_TL_PROVIDER);
    cJSON *res = tlp ? td_call(tlp, "ImportFromXlsx", tda("fe", path, "Siemens.Engineering.ImportOptions", override ? "Override" : "None")) : NULL;
    int rc = res ? xlsx_result(c, res, what) : fail_td(c, what);
    cJSON_Delete(res);
    fs_remove(path);
    return rc;
}

/* Column of a header (exact, case-insensitive) in the first row, or -1. */
static int col_of(const cJSON *rows, const char *header)
{
    const cJSON *h = cJSON_GetArrayItem(rows, 0);
    int i = 0;
    const cJSON *cell;
    cJSON_ArrayForEach(cell, h)
    {
        const char *s = cJSON_GetStringValue(cell);
        if (s && _stricmp(s, header) == 0)
            return i;
        i++;
    }
    return -1;
}

/* "Text [en-US]" column; added (with empty cells) when missing and add is set. */
static int lang_col(cJSON *rows, const char *prefix, const char *lang, int add)
{
    char h[64];
    snprintf(h, sizeof h, "%s [%s]", prefix, lang);
    int col = col_of(rows, h);
    if (col >= 0 || !add)
        return col;
    const cJSON *row;
    int width = cJSON_GetArraySize(cJSON_GetArrayItem(rows, 0));
    int r = 0;
    cJSON_ArrayForEach(row, rows)
    {
        while (cJSON_GetArraySize(row) < width)
            xlsx_add_cell((cJSON *)row, "");
        xlsx_add_cell((cJSON *)row, r++ ? "" : h);
    }
    return width;
}

static void set_cell(cJSON *row, int col, const char *text)
{
    while (cJSON_GetArraySize(row) <= col)
        xlsx_add_cell(row, "");
    cJSON_ReplaceItemInArray(row, col, cJSON_CreateString(text ? text : ""));
}

static int valid_number(const char *s)
{
    if (!s || !*s)
        return 0;
    if (*s == '-')
        s++;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s))
            return 0;
    return 1;
}

/* entries[] item: {from, to?, text?} - numbers may be JSON numbers or strings. */
static const char *entry_field(const cJSON *e, const char *name, char *buf, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, name);
    if (cJSON_IsNumber(v)) {
        snprintf(buf, cap, "%lld", (long long)v->valuedouble);
        return buf;
    }
    return cJSON_GetStringValue(v);
}

/* ---- text lists ---------------------------------------------------------------------------------- */

static int a_list_textlists(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    th group = td_get_h(p.software, "PlcAlarmTextlistGroup");
    cJSON *user = td_enum(td_get_h(group, "PlcAlarmUserTextlists"), "Name,ID,ListRange,Comment", -1);
    out(c, "User text lists of %s:\n", p.device_name);
    const cJSON *it;
    cJSON_ArrayForEach(it, user)
    {
        char *cm = ml_text(tdv_h(tdi_a(it, "Comment")));
        char *id = tdv_text(tdi_a(it, "ID"));
        out(c, "  %s  [id=%s, range=%s%s%s]\n", tdi_s(it, "Name") ? tdi_s(it, "Name") : "?", id ? id : "?",
            tdi_s(it, "ListRange") ? tdi_s(it, "ListRange") : "?", cm && *cm ? ", comment=" : "", cm ? cm : "");
        free(cm);
        free(id);
    }
    if (!cJSON_GetArraySize(user))
        out(c, "  (none)\n");
    cJSON *sys = td_enum(td_get_h(group, "PlcAlarmSystemTextlists"), "Name,ID", -1);
    strbuf sb;
    sb_init(&sb);
    cJSON_ArrayForEach(it, sys)
    sb_printf(&sb, "%s%s", sb.len ? ", " : "", tdi_s(it, "Name") ? tdi_s(it, "Name") : "?");
    out(c, "System text lists (%d, read-only): %s\n", cJSON_GetArraySize(sys), sb.len ? sb_str(&sb) : "none");
    sb_free(&sb);
    cJSON_Delete(user);
    cJSON_Delete(sys);
    td_clear_err();
    return 0;
}

static int a_get_entries(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    const char *name = arg_req(c, "textlistName");
    tl_ref t;
    if (!name || find_textlist(c, &p, name, &t, 0) != 0)
        return -1;
    cJSON *book = export_book(c, &p, t.name);
    if (!book)
        return -1;
    const cJSON *rows = xlsx_sheet_rows(book, "TextListEntry");
    int cp = col_of(rows, "Parent"), cf = col_of(rows, "From"), ct = col_of(rows, "To");
    const cJSON *header = cJSON_GetArrayItem(rows, 0);
    out(c, "Text list %s  [id=%s, range=%s%s]\n", t.name, t.id, t.range, t.system ? ", system" : "");
    int n = 0;
    const cJSON *row;
    int first = 1;
    cJSON_ArrayForEach(row, rows)
    {
        if (first) {
            first = 0;
            continue;
        }
        if (cp >= 0 && _stricmp(xlsx_cell(row, cp), t.name) != 0)
            continue;
        const char *from = xlsx_cell(row, cf), *to = xlsx_cell(row, ct);
        strbuf texts;
        sb_init(&texts);
        int col = 0;
        const cJSON *h;
        cJSON_ArrayForEach(h, header)
        {
            const char *hs = cJSON_GetStringValue(h);
            if (hs && _strnicmp(hs, "Text [", 6) == 0 && *xlsx_cell(row, col))
                sb_printf(&texts, "%s%s%s", texts.len ? " | " : "", cJSON_GetArraySize(header) > 4 ? hs + 5 : "", xlsx_cell(row, col));
            col++;
        }
        if (strcmp(from, to) == 0 || !*to)
            out(c, "  %s: %s\n", from, sb_str(&texts));
        else
            out(c, "  %s..%s: %s\n", from, to, sb_str(&texts));
        sb_free(&texts);
        n++;
    }
    out(c, "%d entr%s.\n", n, n == 1 ? "y" : "ies");
    cJSON_Delete(book);
    return 0;
}

static int a_create_textlist(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    const char *name = arg_req(c, "textlistName");
    if (!name)
        return -1;
    tl_ref t;
    if (find_textlist(c, &p, name, &t, 1) == 0)
        return fail(c, "text list '%s' already exists in %s: use add_entries / update_entries", t.name, p.device_name);
    const char *range = arg_s(c, "listRange");
    if (!range || !*range)
        range = "Decimal";
    if (_stricmp(range, "Decimal") != 0 && _stricmp(range, "Binary") != 0 && _stricmp(range, "Bit") != 0)
        return fail(c, "listRange must be Decimal, Binary or Bit");
    char lb[32];
    const char *lang = language(c, lb, sizeof lb);
    cJSON *book = cJSON_CreateArray();
    cJSON *lists = xlsx_add_sheet(book, "TextList");
    cJSON *h = xlsx_add_row(lists);
    char col[64];
    xlsx_add_cell(h, "Name");
    xlsx_add_cell(h, "ListRange");
    snprintf(col, sizeof col, "Comment [%s]", lang);
    xlsx_add_cell(h, col);
    cJSON *r = xlsx_add_row(lists);
    xlsx_add_cell(r, name);
    xlsx_add_cell(r, range);
    xlsx_add_cell(r, arg_s(c, "comment") ? arg_s(c, "comment") : "");
    cJSON *entries = xlsx_add_sheet(book, "TextListEntry");
    h = xlsx_add_row(entries);
    xlsx_add_cell(h, "Parent");
    xlsx_add_cell(h, "From");
    xlsx_add_cell(h, "To");
    snprintf(col, sizeof col, "Text [%s]", lang);
    xlsx_add_cell(h, col);
    const cJSON *in = arg_arr(c, "entries");
    const cJSON *e;
    int n = 0;
    cJSON_ArrayForEach(e, in)
    {
        char fb[32], tb[32];
        const char *from = entry_field(e, "from", fb, sizeof fb);
        const char *to = entry_field(e, "to", tb, sizeof tb);
        if (!valid_number(from) || (to && *to && !valid_number(to))) {
            cJSON_Delete(book);
            return fail(c, "entries[%d]: from/to must be integers", n);
        }
        r = xlsx_add_row(entries);
        xlsx_add_cell(r, name);
        xlsx_add_cell(r, from);
        xlsx_add_cell(r, to && *to ? to : from);
        xlsx_add_cell(r, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "text")));
        n++;
    }
    int rc = import_book(c, &p, book, 0, "creating the text list failed");
    cJSON_Delete(book);
    if (rc != 0)
        return -1;
    if (find_textlist(c, &p, name, &t, 1) != 0)
        return fail(c, "TIA Portal reported success but text list '%s' does not exist", name);
    out(c, "Text list '%s' created in %s [id=%s, range=%s] with %d entr%s (%s).\n", t.name, p.device_name, t.id, t.range, n,
        n == 1 ? "y" : "ies", lang);
    return 0;
}

typedef enum { ED_ADD, ED_UPDATE, ED_DELETE } edit_kind;

static int edit_entries(tool_ctx *c, edit_kind kind)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    const char *name = arg_req(c, "textlistName");
    tl_ref t;
    if (!name || find_textlist(c, &p, name, &t, 0) != 0)
        return -1;
    if (t.system)
        return fail(c, "'%s' is a system text list: it cannot be changed", t.name);
    const cJSON *in = arg_arr(c, kind == ED_DELETE ? "fromValues" : "entries");
    if (!in || !cJSON_GetArraySize(in))
        return fail(c, kind == ED_DELETE ? "fromValues[] is required" : "entries[] is required ({from, to, text})");
    char lb[32];
    const char *lang = language(c, lb, sizeof lb);
    cJSON *book = export_book(c, &p, t.name);
    if (!book)
        return -1;
    cJSON *rows = (cJSON *)xlsx_sheet_rows(book, "TextListEntry");
    if (!rows) {
        cJSON_Delete(book);
        return fail(c, "unexpected workbook from TIA Portal: no TextListEntry sheet");
    }
    int cp = col_of(rows, "Parent"), cf = col_of(rows, "From"), ct = col_of(rows, "To");
    int ctext = lang_col(rows, "Text", lang, kind != ED_DELETE);
    if (cp < 0 || cf < 0 || ct < 0) {
        cJSON_Delete(book);
        return fail(c, "unexpected workbook from TIA Portal: missing Parent/From/To columns");
    }
    int changed = 0;
    const cJSON *e;
    int i = 0;
    cJSON_ArrayForEach(e, in)
    {
        char fb[32], tb[32];
        const char *from = kind == ED_DELETE ? (cJSON_IsNumber(e) ? (snprintf(fb, sizeof fb, "%lld", (long long)e->valuedouble), fb)
                                                                  : cJSON_GetStringValue(e))
                                             : entry_field(e, "from", fb, sizeof fb);
        if (!valid_number(from)) {
            cJSON_Delete(book);
            return fail(c, "%s[%d]: from must be an integer", kind == ED_DELETE ? "fromValues" : "entries", i);
        }
        cJSON *hit = NULL;
        int hit_index = -1, idx = 0;
        cJSON *row;
        cJSON_ArrayForEach(row, rows)
        {
            if (idx++ == 0)
                continue;
            if (_stricmp(xlsx_cell(row, cp), t.name) == 0 && strcmp(xlsx_cell(row, cf), from) == 0) {
                hit = row;
                hit_index = idx - 1;
                break;
            }
        }
        const char *to = kind == ED_DELETE ? NULL : entry_field(e, "to", tb, sizeof tb);
        const char *text = kind == ED_DELETE ? NULL : cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "text"));
        if (to && *to && !valid_number(to)) {
            cJSON_Delete(book);
            return fail(c, "entries[%d]: to must be an integer", i);
        }
        if (kind == ED_ADD) {
            if (hit) {
                cJSON_Delete(book);
                return fail(c, "entry %s already exists in '%s': use update_entries", from, t.name);
            }
            cJSON *nr = xlsx_add_row(rows);
            set_cell(nr, cp, t.name);
            set_cell(nr, cf, from);
            set_cell(nr, ct, to && *to ? to : from);
            set_cell(nr, ctext, text ? text : "");
        } else if (!hit) {
            cJSON_Delete(book);
            return fail(c, "entry %s not found in '%s' (see get_entries)", from, t.name);
        } else if (kind == ED_UPDATE) {
            if (to && *to)
                set_cell(hit, ct, to);
            if (text)
                set_cell(hit, ctext, text);
        } else {
            cJSON_DeleteItemFromArray(rows, hit_index);
        }
        changed++;
        i++;
    }
    int rc = import_book(c, &p, book, 1, "updating the text list failed");
    cJSON_Delete(book);
    if (rc != 0)
        return -1;
    out(c, "%d entr%s %s in text list '%s' of %s%s%s.\n", changed, changed == 1 ? "y" : "ies",
        kind == ED_ADD ? "added" : kind == ED_UPDATE ? "updated" : "deleted", t.name, p.device_name, kind == ED_DELETE ? "" : ", language ",
        kind == ED_DELETE ? "" : lang);
    return 0;
}

static int a_add_entries(tool_ctx *c)
{
    return edit_entries(c, ED_ADD);
}

static int a_update_entries(tool_ctx *c)
{
    return edit_entries(c, ED_UPDATE);
}

static int a_delete_entries(tool_ctx *c)
{
    return edit_entries(c, ED_DELETE);
}

static int a_update_textlist_comment(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    const char *name = arg_req(c, "textlistName");
    const char *comment = name ? arg_s(c, "newComment") : NULL;
    tl_ref t;
    if (!name || find_textlist(c, &p, name, &t, 0) != 0)
        return -1;
    if (!comment)
        return fail(c, "newComment is required (empty string clears it)");
    if (t.system)
        return fail(c, "'%s' is a system text list: it cannot be changed", t.name);
    char lb[32];
    const char *lang = language(c, lb, sizeof lb);
    if (ml_set(td_get_h(t.item, "Comment"), lang, comment) != 0)
        return td_failed() ? fail_td(c, "setting the comment failed") : fail(c, "language %s is not a project language", lang);
    out(c, "Comment of text list '%s' (%s) set.\n", t.name, lang);
    return 0;
}

static int a_delete_textlist(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    const char *name = arg_req(c, "textlistName");
    tl_ref t;
    if (!name || find_textlist(c, &p, name, &t, 0) != 0)
        return -1;
    if (t.system)
        return fail(c, "'%s' is a system text list: it cannot be deleted", t.name);
    if (td_call_v(t.item, "Delete", NULL) != 0)
        return fail_td(c, "deleting the text list failed");
    out(c, "Text list '%s' [id=%s] deleted from %s. Alarms referencing it now show no text.\n", t.name, t.id, p.device_name);
    return 0;
}

/* ---- Excel export / import --------------------------------------------------------------------- */

static void default_name(const nav_plc *p, const char *what, const char *ext, char *out, size_t cap)
{
    char *pn = td_get_s(session_project(), "Name");
    snprintf(out, cap, "%s_%s_%s.%s", pn ? pn : "project", p->device_name, what, ext);
    free(pn);
    for (char *s = out; *s; s++)
        if (strchr("\\/:*?\"<>| ", *s))
            *s = '_';
}

static int deliver(tool_ctx *c, const char *path, const char *name, const char *type)
{
    int rc = sw_deliver(c, path, name, type, arg_s(c, "outputPath"), 0);
    fs_remove(path);
    return rc;
}

static int a_export_textlists(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    th tlp = td_service(p.software, T_TL_PROVIDER);
    if (!tlp)
        return fail_td(c, "the text list service is not available for this PLC");
    char path[TC_PATH_MAX], name[400];
    if (fs_temp_path("textlists", ".xlsx", path, sizeof path) != 0)
        return fail(c, "cannot create a temporary file");
    fs_remove(path);
    const cJSON *names = arg_arr(c, "textListNames");
    cJSON *res = names && cJSON_GetArraySize(names) ? td_call(tlp, "ExportToXlsx", tda("fjn", path, cJSON_Duplicate(names, 1)))
                                                    : td_call(tlp, "ExportToXlsx", tda("f", path));
    if (!res)
        return fail_td(c, "exporting the text lists failed (a PLC without user or system text lists has nothing to export)");
    int rc = xlsx_result(c, res, "exporting the text lists failed");
    cJSON_Delete(res);
    if (rc != 0)
        return -1;
    default_name(&p, "textlists", "xlsx", name, sizeof name);
    return deliver(c, path, name, XLSX_TYPE);
}

static int input_file(tool_ctx *c, char *path, size_t cap)
{
    const char *f = arg_req(c, "filePath");
    if (!f)
        return -1;
    if (fs_full_path(f, path, cap) != 0 || !fs_is_file(path))
        return fail(c, "file '%s' not found", f);
    return 0;
}

static int a_import_textlists(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    char path[TC_PATH_MAX];
    if (input_file(c, path, sizeof path) != 0)
        return -1;
    th tlp = td_service(p.software, T_TL_PROVIDER);
    int ov = arg_b(c, "overwrite", 0);
    cJSON *res = tlp ? td_call(tlp, "ImportFromXlsx", tda("fe", path, "Siemens.Engineering.ImportOptions", ov ? "Override" : "None")) : NULL;
    if (!res)
        return fail_td(c, "importing the text lists failed");
    int rc = xlsx_result(c, res, "importing the text lists failed");
    cJSON_Delete(res);
    if (rc == 0)
        out(c, "Text lists imported into %s from %s%s.\n", p.device_name, path, ov ? " (existing lists replaced)" : "");
    return rc;
}

static int export_alarm_texts_to(tool_ctx *c, const nav_plc *p, const char *path)
{
    th atp = td_service(p->software, T_TEXT_PROVIDER);
    if (!atp)
        return fail_td(c, "the alarm text service is not available for this PLC");
    fs_remove(path);
    const char *opt = arg_b(c, "includeInfoText", 0) ? "All" : "IncludeAdditionalTexts, IncludeAlarmClass";
    cJSON *res = td_call(atp, "ExportInstanceTextsToXlsx", tda("fne", path, "Siemens.Engineering.SW.Alarm.PlcAlarmTextXlsxExportOption", opt));
    if (!res)
        return fail_td(c, "exporting the alarm texts failed (a PLC without alarms has nothing to export)");
    int rc = xlsx_result(c, res, "exporting the alarm texts failed");
    cJSON_Delete(res);
    return rc;
}

static int a_export_alarm_texts(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    char path[TC_PATH_MAX], name[400];
    if (fs_temp_path("alarmtexts", ".xlsx", path, sizeof path) != 0)
        return fail(c, "cannot create a temporary file");
    if (export_alarm_texts_to(c, &p, path) != 0)
        return -1;
    default_name(&p, "alarm_texts", "xlsx", name, sizeof name);
    return deliver(c, path, name, XLSX_TYPE);
}

static int a_import_alarm_texts(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    char path[TC_PATH_MAX];
    if (input_file(c, path, sizeof path) != 0)
        return -1;
    th atp = td_service(p.software, T_TEXT_PROVIDER);
    cJSON *res = atp ? td_call(atp, "ImportInstanceTextsFromXlsx", tda("fn", path)) : NULL;
    if (!res)
        return fail_td(c, "importing the alarm texts failed");
    int rc = xlsx_result(c, res, "importing the alarm texts failed");
    cJSON_Delete(res);
    if (rc == 0)
        out(c, "Alarm instance texts imported into %s from %s.\n", p.device_name, path);
    return rc;
}

/* Alarm classes: TIA exports a .dat file (zip with an XML document). */
static int export_classes_to(tool_ctx *c, const char *path, cJSON **classes)
{
    th acp = td_service(session_project(), T_CLASS_PROVIDER);
    if (!acp)
        return fail_td(c, "the alarm class service is not available");
    fs_remove(path);
    cJSON *res = td_call(acp, "Export", tda("f", path));
    if (!res)
        return fail_td(c, "exporting the alarm classes failed");
    char *state = td_get_s(tdv_h(res), "State");
    int ok = state && strcmp(state, "Error") != 0;
    free(state);
    cJSON_Delete(res);
    if (!ok)
        return fail(c, "exporting the alarm classes failed");
    if (classes) {
        *classes = cJSON_CreateArray();
        char dir[TC_PATH_MAX], err[256];
        if (fs_temp_dir("alarmclasses", dir, sizeof dir) == 0) {
            fs_remove_tree(dir);
            if (zip_extract(path, dir, err, sizeof err) == 0) {
                char xml[TC_PATH_MAX];
                fs_join(xml, sizeof xml, dir, fs_basename(path));
                mxml_node_t *top = sml_load_file(xml, err, sizeof err);
                mxml_node_t *root = top ? sml_find(top, "AlarmServiceGlobalSettingsData") : NULL;
                for (mxml_node_t *a = sml_child(root, "AlarmClass"); a; a = sml_next(a, "AlarmClass")) {
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddStringToObject(o, "Name", sml_attr(a, "Name") ? sml_attr(a, "Name") : "");
                    cJSON_AddStringToObject(o, "DisplayName", sml_child_text(a, "DisplayName") ? sml_child_text(a, "DisplayName") : "");
                    cJSON_AddStringToObject(o, "ShortName", sml_child_text(a, "ShortName") ? sml_child_text(a, "ShortName") : "");
                    cJSON_AddStringToObject(o, "GlobalId", sml_child_text(a, "GlobalId") ? sml_child_text(a, "GlobalId") : "");
                    cJSON_AddItemToArray(*classes, o);
                }
                if (top)
                    mxmlDelete(top);
            }
            fs_remove_tree(dir);
        }
    }
    return 0;
}

static int a_export_alarm_classes(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    char path[TC_PATH_MAX], name[400];
    if (fs_temp_path("alarmclasses", ".dat", path, sizeof path) != 0)
        return fail(c, "cannot create a temporary file");
    cJSON *classes = NULL;
    if (export_classes_to(c, path, &classes) != 0)
        return -1;
    out(c, "Alarm classes of the project (common data, shared by all PLCs):\n");
    const cJSON *it;
    cJSON_ArrayForEach(it, classes)
    out(c, "  %s  [display=%s, short=%s, id=%s]\n", cJSON_GetStringValue(cJSON_GetObjectItem(it, "Name")),
        cJSON_GetStringValue(cJSON_GetObjectItem(it, "DisplayName")), cJSON_GetStringValue(cJSON_GetObjectItem(it, "ShortName")),
        cJSON_GetStringValue(cJSON_GetObjectItem(it, "GlobalId")));
    cJSON_Delete(classes);
    default_name(&p, "alarm_classes", "dat", name, sizeof name);
    return deliver(c, path, name, "application/octet-stream");
}

static int a_import_alarm_classes(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    char path[TC_PATH_MAX];
    if (input_file(c, path, sizeof path) != 0)
        return -1;
    th acp = td_service(session_project(), T_CLASS_PROVIDER);
    cJSON *res = acp ? td_call(acp, "Import", tda("f", path)) : NULL;
    if (!res)
        return fail_td(c, "importing the alarm classes failed (TIA expects a .dat file made by export_alarm_classes)");
    th r = tdv_h(res);
    char *state = td_get_s(r, "State");
    cJSON *msgs = td_enum(td_get_h(r, "Messages"), "Message,State", -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, msgs)
    out(c, "  %s: %s\n", tdi_s(it, "State") ? tdi_s(it, "State") : "?", tdi_s(it, "Message") ? tdi_s(it, "Message") : "");
    cJSON_Delete(msgs);
    int ok = state && strcmp(state, "Error") != 0;
    int rc = ok ? 0 : fail(c, "importing the alarm classes failed: %s", state ? state : "?");
    if (ok)
        out(c, "Alarm classes imported from %s (%s).\n", path, state);
    free(state);
    cJSON_Delete(res);
    td_clear_err();
    return rc;
}

/* ---- export_alarm_data ---------------------------------------------------------------------------- */

static void book_to_csv(const cJSON *book, strbuf *sb)
{
    int nsheets = cJSON_GetArraySize(book);
    const cJSON *s;
    cJSON_ArrayForEach(s, book)
    {
        const char *sn = cJSON_GetStringValue(cJSON_GetObjectItem(s, "name"));
        const cJSON *row;
        int r = 0;
        cJSON_ArrayForEach(row, cJSON_GetObjectItem(s, "rows"))
        {
            if (nsheets > 1) {
                csv_field(sb, r == 0 ? "Sheet" : sn);
                sb_appendc(sb, ';');
            }
            const cJSON *cell;
            int i = 0;
            cJSON_ArrayForEach(cell, row)
            {
                if (i++)
                    sb_appendc(sb, ';');
                csv_field(sb, cJSON_GetStringValue(cell));
            }
            sb_append(sb, "\r\n");
            r++;
        }
    }
}

/* Text lists as one flat table: list columns repeated on each entry. */
static void textlists_flat(const cJSON *book, cJSON *out)
{
    const cJSON *lists = xlsx_sheet_rows(book, "TextList");
    const cJSON *entries = xlsx_sheet_rows(book, "TextListEntry");
    cJSON *rows = xlsx_add_sheet(out, "TextLists");
    cJSON *h = xlsx_add_row(rows);
    const cJSON *lh = cJSON_GetArrayItem(lists, 0), *eh = cJSON_GetArrayItem(entries, 0);
    const cJSON *cell;
    cJSON_ArrayForEach(cell, lh)
    xlsx_add_cell(h, cJSON_GetStringValue(cell));
    int i = 0;
    cJSON_ArrayForEach(cell, eh)
    if (i++ > 0) /* skip Parent */
        xlsx_add_cell(h, cJSON_GetStringValue(cell));
    int cname = col_of(lists, "Name"), cparent = col_of(entries, "Parent");
    int li = 0;
    const cJSON *l;
    cJSON_ArrayForEach(l, lists)
    {
        if (li++ == 0)
            continue;
        int ei = 0, any = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, entries)
        {
            if (ei++ == 0 || _stricmp(xlsx_cell(e, cparent), xlsx_cell(l, cname)) != 0)
                continue;
            cJSON *r = cJSON_Duplicate(l, 1);
            while (cJSON_GetArraySize(r) < cJSON_GetArraySize(lh))
                xlsx_add_cell(r, "");
            int k = 0;
            cJSON_ArrayForEach(cell, e)
            if (k++ > 0)
                xlsx_add_cell(r, cJSON_GetStringValue(cell));
            cJSON_AddItemToArray(rows, r);
            any = 1;
        }
        if (!any)
            cJSON_AddItemToArray(rows, cJSON_Duplicate(l, 1));
    }
}

static int a_export_alarm_data(tool_ctx *c)
{
    nav_plc p;
    if (sw_plc(c, &p) != 0)
        return -1;
    const char *format = arg_s(c, "format");
    if (!format || !*format)
        format = "csv";
    int xlsx = _stricmp(format, "xlsx") == 0;
    if (!xlsx && _stricmp(format, "csv") != 0)
        return fail(c, "format must be csv or xlsx");
    const char *scope = arg_s(c, "scope");
    if (!scope || !*scope)
        scope = "textlists";
    cJSON *book = NULL;
    char path[TC_PATH_MAX], err[256], name[400];
    if (_stricmp(scope, "textlists") == 0) {
        cJSON *raw = export_book(c, &p, NULL);
        if (!raw)
            return -1;
        book = cJSON_CreateArray();
        textlists_flat(raw, book);
        cJSON_Delete(raw);
    } else if (_stricmp(scope, "alarm_texts") == 0) {
        if (fs_temp_path("alarmtexts", ".xlsx", path, sizeof path) != 0 || export_alarm_texts_to(c, &p, path) != 0)
            return c->is_error ? -1 : fail(c, "cannot create a temporary file");
        book = xlsx_read(path, err, sizeof err);
        fs_remove(path);
        if (!book)
            return fail(c, "%s", err);
    } else if (_stricmp(scope, "alarm_classes") == 0) {
        cJSON *classes = NULL;
        if (fs_temp_path("alarmclasses", ".dat", path, sizeof path) != 0 || export_classes_to(c, path, &classes) != 0)
            return c->is_error ? -1 : fail(c, "cannot create a temporary file");
        fs_remove(path);
        book = cJSON_CreateArray();
        cJSON *rows = xlsx_add_sheet(book, "AlarmClasses");
        cJSON *h = xlsx_add_row(rows);
        xlsx_add_cell(h, "Name");
        xlsx_add_cell(h, "DisplayName");
        xlsx_add_cell(h, "ShortName");
        xlsx_add_cell(h, "GlobalId");
        const cJSON *it;
        cJSON_ArrayForEach(it, classes)
        {
            cJSON *r = xlsx_add_row(rows);
            xlsx_add_cell(r, cJSON_GetStringValue(cJSON_GetObjectItem(it, "Name")));
            xlsx_add_cell(r, cJSON_GetStringValue(cJSON_GetObjectItem(it, "DisplayName")));
            xlsx_add_cell(r, cJSON_GetStringValue(cJSON_GetObjectItem(it, "ShortName")));
            xlsx_add_cell(r, cJSON_GetStringValue(cJSON_GetObjectItem(it, "GlobalId")));
        }
        cJSON_Delete(classes);
    } else {
        return fail(c, "scope must be textlists, alarm_texts or alarm_classes");
    }
    int nrows = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, book)
    nrows += cJSON_GetArraySize(cJSON_GetObjectItem(s, "rows")) - 1;
    char what[64];
    snprintf(what, sizeof what, "%s_table", scope); /* not the name of TIA's own export files */
    default_name(&p, what, xlsx ? "xlsx" : "csv", name, sizeof name);
    int rc;
    if (xlsx) {
        if (fs_temp_path("alarmdata", ".xlsx", path, sizeof path) != 0 || xlsx_write(path, book, NULL, err, sizeof err) != 0)
            rc = fail(c, "cannot write the workbook: %s", err);
        else {
            out(c, "%d row(s).\n", nrows);
            rc = deliver(c, path, name, XLSX_TYPE);
        }
    } else {
        strbuf sb;
        sb_init(&sb);
        sb_append(&sb, "\xEF\xBB\xBF");
        book_to_csv(book, &sb);
        out(c, "%d row(s).\n", nrows);
        if (fs_temp_path("alarmdata", ".csv", path, sizeof path) != 0 || fs_write_all(path, sb.p, sb.len) != 0)
            rc = fail(c, "cannot write the CSV");
        else {
            const char *outp = arg_s(c, "outputPath");
            rc = sw_deliver(c, path, name, "text/csv", outp, arg_b(c, "returnInline", outp && *outp ? 0 : 1));
            fs_remove(path);
        }
        sb_free(&sb);
    }
    cJSON_Delete(book);
    return rc;
}

/* ---- tool ------------------------------------------------------------------------------------------- */

#define R AF_PROJECT
#define W (AF_PROJECT | AF_WRITES)

static const action_def actions[] = {
    { "add_entries", "deviceName, textlistName, entries[]; optional language",
      "Add entries {from, to?, text} to a user text list (to defaults to from). An existing From is refused (use "
      "update_entries). language defaults to the project editing language.",
      a_add_entries, W },
    { "create_textlist", "deviceName, textlistName; optional listRange=Decimal|Binary|Bit, comment, entries[], language",
      "Create a user text list, optionally with entries {from, to?, text}. Works on PLCs without any text list.",
      a_create_textlist, W },
    { "delete_entries", "deviceName, textlistName, fromValues[]", "Delete entries of a user text list by their From value.",
      a_delete_entries, W | AF_DESTRUCTIVE },
    { "delete_textlist", "deviceName, textlistName", "Delete a user text list. System text lists are refused.", a_delete_textlist,
      W | AF_DESTRUCTIVE },
    { "export_alarm_classes", "deviceName; optional outputPath",
      "List the project's alarm classes and export them as TIA's .dat file (for import_alarm_classes in another project). "
      "Alarm classes are common data of the project.",
      a_export_alarm_classes, R },
    { "export_alarm_data", "deviceName; optional format=csv|xlsx, scope=textlists|alarm_texts|alarm_classes, outputPath, includeInfoText, returnInline",
      "Alarm data as one table for documentation: textlists (one row per entry with its list), alarm_texts (alarm "
      "instance texts) or alarm_classes. After export, ask the user whether to open the file (admin action=open_file).",
      a_export_alarm_data, R },
    { "export_alarm_texts", "deviceName; optional outputPath, includeInfoText=false",
      "Export the alarm instance texts of the PLC (Program_Alarm, ProDiag, ...) as TIA's Excel file, the format "
      "import_alarm_texts reads back. After export, ask the user whether to open the file.",
      a_export_alarm_texts, R },
    { "export_textlists", "deviceName; optional outputPath, textListNames[]",
      "Export text lists (all, or the named ones) as TIA's Excel file (sheets TextList and TextListEntry). After export, ask "
      "the user whether to open the file.",
      a_export_textlists, R },
    { "get_entries", "deviceName, textlistName", "Entries of a text list: From..To and text per project language.", a_get_entries, R },
    { "import_alarm_classes", "deviceName, filePath", "Import alarm classes from a .dat file made by export_alarm_classes.",
      a_import_alarm_classes, W | AF_NO_TX },
    { "import_alarm_texts", "deviceName, filePath", "Import alarm instance texts from an Excel file in TIA's export format.",
      a_import_alarm_texts, W },
    { "import_textlists", "deviceName, filePath; optional overwrite=false",
      "Import text lists from an Excel file in TIA's format. overwrite=true replaces lists that exist (their entries "
      "become exactly those in the file).",
      a_import_textlists, W },
    { "list_textlists", "deviceName", "User text lists (name, ID, range, comment) and the names of the system text lists.",
      a_list_textlists, R },
    { "update_entries", "deviceName, textlistName, entries[]; optional language",
      "Update entries by their From value: {from, to?, text?}. language defaults to the project editing language.",
      a_update_entries, W },
    { "update_textlist_comment", "deviceName, textlistName, newComment; optional language",
      "Set the comment of a user text list (direct API, no Excel round trip).", a_update_textlist_comment, W },
};

const tool_def tool_alarm_text = {
    .name = "alarm_text",
    .title = "Alarm texts & text lists",
    .summary = "PLC alarm text lists and entries, alarm instance texts and alarm classes. Openness exposes text list "
               "entries only through TIA's Excel import/export: entry changes export the list, edit it and import it "
               "again (one undo step).",
    .properties =
        "{"
        "\"deviceName\":{\"type\":\"string\",\"description\":\"Device or PLC name (session list_devices).\"},"
        "\"textlistName\":{\"type\":\"string\"},"
        "\"listRange\":{\"type\":\"string\",\"enum\":[\"Decimal\",\"Binary\",\"Bit\"]},"
        "\"comment\":{\"type\":\"string\"},\"newComment\":{\"type\":\"string\"},"
        "\"entries\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"from\":{\"type\":[\"integer\",\"string\"]},"
        "\"to\":{\"type\":[\"integer\",\"string\"]},\"text\":{\"type\":\"string\"}}}},"
        "\"fromValues\":{\"type\":\"array\",\"items\":{\"type\":[\"integer\",\"string\"]}},"
        "\"language\":{\"type\":\"string\",\"description\":\"Culture such as en-US (default: the project editing language).\"},"
        "\"textListNames\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"outputPath\":{\"type\":\"string\"},\"filePath\":{\"type\":\"string\"},\"overwrite\":{\"type\":\"boolean\"},"
        "\"includeInfoText\":{\"type\":\"boolean\"},\"returnInline\":{\"type\":\"boolean\"},"
        "\"format\":{\"type\":\"string\",\"enum\":[\"csv\",\"xlsx\"]},"
        "\"scope\":{\"type\":\"string\",\"enum\":[\"textlists\",\"alarm_texts\",\"alarm_classes\"]}"
        "}",
    .actions = actions,
    .nactions = COUNT_OF(actions),
};
