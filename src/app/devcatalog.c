#include "devcatalog.h"

#include "config.h"
#include "tia/tia_env.h"
#include "util/fs.h"

#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void dc_catalog_path(char *out, size_t cap)
{
    char dir[TC_PATH_MAX];
    fs_join(dir, sizeof dir, g_cfg.data_dir, "catalog");
    fs_mkdirs(dir);
    fs_join(out, cap, dir, "hardware_catalog.tsv");
}

void dc_clean_field(char *s)
{
    for (; s && *s; s++)
        if (*s == '\t' || *s == '\r' || *s == '\n')
            *s = ' ';
}

int dc_catalog_write(const char *data, size_t len)
{
    char path[TC_PATH_MAX], tmp[TC_PATH_MAX];
    dc_catalog_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (fs_write_all(tmp, data, len) != 0)
        return -1;
    return fs_move(tmp, path);
}

int dc_catalog_delete(void)
{
    char path[TC_PATH_MAX];
    dc_catalog_path(path, sizeof path);
    return fs_is_file(path) ? fs_remove(path) : 0;
}

/* Case-insensitive substring search. */
static int contains_ci(const char *hay, size_t hay_len, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return 1;
    for (size_t i = 0; i + n <= hay_len; i++)
        if (_strnicmp(hay + i, needle, n) == 0)
            return 1;
    return 0;
}

void dc_format_entry(strbuf *out, const char *article, const char *version, const char *type_name,
                     const char *type_id, const char *path)
{
    /* "Root\Controllers\SIMATIC S7-1500\DI\..." -> "Controllers/SIMATIC S7-1500/DI/..." */
    char p[600];
    snprintf(p, sizeof p, "%s", path ? path : "");
    char *s = p;
    if (_strnicmp(s, "Root\\", 5) == 0)
        s += 5;
    for (char *q = s; *q; q++)
        if (*q == '\\')
            *q = '/';
    sb_printf(out, "%s  [article=%s, version=%s, typeIdentifier=%s, path=%s]\n", type_name && *type_name ? type_name : "?",
              article && *article ? article : "-", version && *version ? version : "-", type_id ? type_id : "?", s);
}

int dc_catalog_search(const char *filter, int limit, strbuf *out, char *info, size_t info_cap)
{
    char path[TC_PATH_MAX];
    dc_catalog_path(path, sizeof path);
    char *data = NULL;
    size_t len = 0;
    if (info && info_cap)
        info[0] = 0;
    if (!fs_is_file(path) || fs_read_all(path, &data, &len) != 0)
        return -1;
    char terms[16][128];
    int nterms = 0;
    for (const char *p = filter ? filter : ""; *p && nterms < 16;) {
        while (*p && isspace((unsigned char)*p))
            p++;
        size_t n = 0;
        while (p[n] && !isspace((unsigned char)p[n]))
            n++;
        if (n) {
            snprintf(terms[nterms++], sizeof terms[0], "%.*s", (int)(n < 127 ? n : 127), p);
            p += n;
        }
    }
    int matches = 0;
    char *line = data;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t ll = nl ? (size_t)(nl - line) : strlen(line);
        if (ll && line[ll - 1] == '\r')
            ll--;
        if (line[0] == '#') {
            if (info && info_cap && !info[0])
                snprintf(info, info_cap, "%.*s", (int)(ll > 1 ? ll - 1 : 0), line + 1);
        } else if (ll) {
            int ok = 1;
            for (int i = 0; i < nterms && ok; i++)
                ok = contains_ci(line, ll, terms[i]);
            if (ok) {
                matches++;
                if (matches <= limit) {
                    char buf[2048];
                    snprintf(buf, sizeof buf, "%.*s", (int)(ll < sizeof buf - 1 ? ll : sizeof buf - 1), line);
                    char *f[6] = { 0 };
                    char *q = buf;
                    for (int i = 0; i < 6 && q; i++) {
                        f[i] = q;
                        q = strchr(q, '\t');
                        if (q)
                            *q++ = 0;
                    }
                    dc_format_entry(out, f[0], f[1], f[2], f[3], f[4]);
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(data);
    return matches;
}

/* ---- device profiles ---------------------------------------------------------------------- */

typedef struct profile {
    char kind[16];
    char type_name[128];
    char order[64];
    char fw[32];
    char tia[16];
    char first[16];
    char last[16];
} profile;

static SRWLOCK g_plock = SRWLOCK_INIT;
static profile *g_prof;
static int g_nprof, g_capprof, g_loaded;

static void profiles_path(char *out, size_t cap)
{
    fs_join(out, cap, g_cfg.data_dir, "device_profiles.tsv");
}

static void today(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_s(&tm, &t);
    strftime(out, cap, "%Y-%m-%d", &tm);
}

static void profiles_load(void)
{
    if (g_loaded)
        return;
    g_loaded = 1;
    char path[TC_PATH_MAX];
    profiles_path(path, sizeof path);
    char *data = NULL;
    size_t len = 0;
    if (fs_read_all(path, &data, &len) != 0)
        return;
    for (char *line = strtok(data, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (*line == '#')
            continue;
        char *f[7] = { 0 };
        char *q = line;
        for (int i = 0; i < 7 && q; i++) {
            f[i] = q;
            q = strchr(q, '\t');
            if (q)
                *q++ = 0;
        }
        if (!f[6])
            continue;
        if (g_nprof == g_capprof) {
            int cap = g_capprof ? g_capprof * 2 : 32;
            profile *p = realloc(g_prof, (size_t)cap * sizeof *p);
            if (!p)
                break;
            g_prof = p;
            g_capprof = cap;
        }
        profile *p = &g_prof[g_nprof++];
        snprintf(p->kind, sizeof p->kind, "%s", f[0]);
        snprintf(p->type_name, sizeof p->type_name, "%s", f[1]);
        snprintf(p->order, sizeof p->order, "%s", f[2]);
        snprintf(p->fw, sizeof p->fw, "%s", f[3]);
        snprintf(p->tia, sizeof p->tia, "%s", f[4]);
        snprintf(p->first, sizeof p->first, "%s", f[5]);
        snprintf(p->last, sizeof p->last, "%s", f[6]);
    }
    free(data);
}

static void profiles_save(void)
{
    strbuf sb;
    sb_init(&sb);
    sb_append(&sb, "# kind\ttypeName\torderNumber\tfirmware\ttiaVersion\tfirstSeen\tlastSeen\n");
    for (int i = 0; i < g_nprof; i++) {
        const profile *p = &g_prof[i];
        sb_printf(&sb, "%s\t%s\t%s\t%s\t%s\t%s\t%s\n", p->kind, p->type_name, p->order, p->fw, p->tia, p->first, p->last);
    }
    char path[TC_PATH_MAX], tmp[TC_PATH_MAX];
    profiles_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (fs_write_all(tmp, sb.p, sb.len) == 0)
        fs_move(tmp, path);
    sb_free(&sb);
}

void dc_profile_seen(const char *kind, const char *type_name, const char *order, const char *firmware)
{
    if (!order || !*order)
        return;
    char day[16];
    today(day, sizeof day);
    AcquireSRWLockExclusive(&g_plock);
    profiles_load();
    int changed = 0, found = 0;
    for (int i = 0; i < g_nprof && !found; i++) {
        profile *p = &g_prof[i];
        if (strcmp(p->order, order) == 0 && strcmp(p->fw, firmware ? firmware : "") == 0) {
            found = 1;
            if (strcmp(p->last, day) != 0) {
                snprintf(p->last, sizeof p->last, "%s", day);
                changed = 1;
            }
        }
    }
    if (!found) {
        if (g_nprof == g_capprof) {
            int cap = g_capprof ? g_capprof * 2 : 32;
            profile *np = realloc(g_prof, (size_t)cap * sizeof *np);
            if (np) {
                g_prof = np;
                g_capprof = cap;
            }
        }
        if (g_nprof < g_capprof) {
            profile *p = &g_prof[g_nprof++];
            memset(p, 0, sizeof *p);
            snprintf(p->kind, sizeof p->kind, "%s", kind ? kind : "");
            snprintf(p->type_name, sizeof p->type_name, "%s", type_name ? type_name : "");
            snprintf(p->order, sizeof p->order, "%s", order);
            snprintf(p->fw, sizeof p->fw, "%s", firmware ? firmware : "");
            tia_portal_version(p->tia, sizeof p->tia);
            dc_clean_field(p->type_name);
            snprintf(p->first, sizeof p->first, "%s", day);
            snprintf(p->last, sizeof p->last, "%s", day);
            changed = 1;
        }
    }
    if (changed)
        profiles_save();
    ReleaseSRWLockExclusive(&g_plock);
}

int dc_profiles_list(strbuf *out)
{
    AcquireSRWLockExclusive(&g_plock);
    profiles_load();
    for (int i = 0; i < g_nprof; i++) {
        const profile *p = &g_prof[i];
        sb_printf(out, "%s  [kind=%s, order=%s, firmware=%s, tia=%s, firstSeen=%s, lastSeen=%s]\n",
                  *p->type_name ? p->type_name : "?", p->kind, p->order, *p->fw ? p->fw : "-", *p->tia ? p->tia : "?",
                  p->first, p->last);
    }
    int n = g_nprof;
    ReleaseSRWLockExclusive(&g_plock);
    return n;
}
