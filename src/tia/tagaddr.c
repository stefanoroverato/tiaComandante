#include "tagaddr.h"

#include "tia/tia_sw.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ta_parse(const char *a, tag_span *s)
{
    if (!a)
        return -1;
    while (*a == ' ')
        a++;
    if (*a == '%')
        a++;
    char area = (char)toupper((unsigned char)*a);
    if (area == 'E')
        area = 'I';
    if (area == 'A')
        area = 'Q';
    if (area != 'M' && area != 'I' && area != 'Q')
        return -1;
    a++;
    int size = 0;
    char unit = (char)toupper((unsigned char)*a);
    if (unit == 'B' || unit == 'W' || unit == 'D' || unit == 'L') {
        size = unit == 'B' ? 1 : unit == 'W' ? 2 : unit == 'D' ? 4 : 8;
        a++;
    } else if (unit == 'X') {
        a++;
    }
    if (!isdigit((unsigned char)*a))
        return -1;
    char *end;
    long byte = strtol(a, &end, 10);
    s->area = area;
    s->byte = byte;
    s->size = size;
    s->bit = -1;
    if (!size) {
        if (*end != '.')
            return -1;
        s->bit = (int)strtol(end + 1, NULL, 10);
    }
    return 0;
}

int ta_type_size(const char *t, int *is_bit)
{
    *is_bit = 0;
    if (!t || _stricmp(t, "Bool") == 0) {
        *is_bit = 1;
        return 0;
    }
    static const struct {
        const char *n;
        int s;
    } sizes[] = { { "Byte", 1 },  { "Char", 1 },  { "SInt", 1 },  { "USInt", 1 }, { "Word", 2 },  { "Int", 2 },
                  { "UInt", 2 },  { "WChar", 2 }, { "Date", 2 },  { "S5Time", 2 }, { "DWord", 4 }, { "DInt", 4 },
                  { "UDInt", 4 }, { "Real", 4 },  { "Time", 4 },  { "TOD", 4 },   { "Time_Of_Day", 4 },
                  { "LWord", 8 }, { "LInt", 8 },  { "ULInt", 8 }, { "LReal", 8 }, { "LTime", 8 } };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        if (_stricmp(t, sizes[i].n) == 0)
            return sizes[i].s;
    return -1;
}

typedef struct collect_ctx {
    tag_spans *sp;
    int used_only;
} collect_ctx;

static int tag_is_used(th tag)
{
    th svc = td_service(tag, "Siemens.Engineering.CrossReference.CrossReferenceService");
    th res = svc ? td_call_h(svc, "GetCrossReferences", tda("e", "Siemens.Engineering.CrossReference.CrossReferenceFilter", "AllObjects")) : 0;
    cJSON *srcs = res ? td_enum(td_get_h(res, "Sources"), NULL, -1) : NULL;
    int used = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, srcs)
    {
        cJSON *refs = td_enum(td_get_h(tdv_h(s), "References"), NULL, -1);
        used |= cJSON_GetArraySize(refs) > 0;
        cJSON_Delete(refs);
    }
    cJSON_Delete(srcs);
    td_clear_err();
    return used;
}

static int collect_cb(void *ctx, const cJSON *item, const char *folder)
{
    (void)folder;
    collect_ctx *k = ctx;
    cJSON *tags = td_enum(td_get_h(tdv_h(item), "Tags"), "Name,DataTypeName,LogicalAddress", -1);
    const cJSON *it;
    cJSON_ArrayForEach(it, tags)
    {
        tag_span s;
        memset(&s, 0, sizeof s);
        if (ta_parse(tdi_s(it, "LogicalAddress"), &s) != 0)
            continue;
        if (s.size == 0 && s.bit < 0)
            continue;
        if (s.bit >= 0) {
            int is_bit;
            int sz = ta_type_size(tdi_s(it, "DataTypeName"), &is_bit);
            if (!is_bit && sz > 0) {
                s.size = sz;
                s.bit = -1;
            }
        }
        if (k->used_only && !tag_is_used(tdv_h(it)))
            continue;
        snprintf(s.tag, sizeof s.tag, "%s", tdi_s(it, "Name") ? tdi_s(it, "Name") : "?");
        snprintf(s.table, sizeof s.table, "%s", tdi_s(item, "Name") ? tdi_s(item, "Name") : "?");
        snprintf(s.type, sizeof s.type, "%s", tdi_s(it, "DataTypeName") ? tdi_s(it, "DataTypeName") : "");
        if (k->sp->n == k->sp->cap) {
            int cap = k->sp->cap ? k->sp->cap * 2 : 128;
            tag_span *p = realloc(k->sp->v, (size_t)cap * sizeof *p);
            if (!p)
                break;
            k->sp->v = p;
            k->sp->cap = cap;
        }
        k->sp->v[k->sp->n++] = s;
    }
    cJSON_Delete(tags);
    return 0;
}

static int span_cmp(const void *a, const void *b)
{
    const tag_span *x = a, *y = b;
    if (x->area != y->area)
        return x->area - y->area;
    if (x->byte != y->byte)
        return x->byte < y->byte ? -1 : 1;
    return x->bit - y->bit;
}

int ta_overlaps(const tag_span *a, const tag_span *b)
{
    if (a->area != b->area)
        return 0;
    long a0 = a->byte, a1 = a->byte + (a->size ? a->size - 1 : 0);
    long b0 = b->byte, b1 = b->byte + (b->size ? b->size - 1 : 0);
    if (a1 < b0 || b1 < a0)
        return 0;
    if (a->bit >= 0 && b->bit >= 0)
        return a->bit == b->bit && a->byte == b->byte;
    return 1;
}

int ta_load(th plc_software, tag_spans *sp, int used_only)
{
    memset(sp, 0, sizeof *sp);
    collect_ctx k = { sp, used_only };
    sw_walk(plc_software, SWC_TAG_TABLES, "Name", collect_cb, NULL, &k);
    if (sp->n > 1)
        qsort(sp->v, (size_t)sp->n, sizeof *sp->v, span_cmp);
    return 0;
}

void ta_free(tag_spans *sp)
{
    free(sp->v);
    memset(sp, 0, sizeof *sp);
}
