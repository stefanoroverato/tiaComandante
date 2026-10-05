/* Absolute addresses of PLC tags (%I, %Q, %M): parsing and per-PLC collection,
   shared by the tag tool (occupancy) and the hardware tool (I/O map). */
#ifndef TC_TAGADDR_H
#define TC_TAGADDR_H

#include "tia/tia_dyn.h"

typedef struct tag_span {
    char area; /* M, I, Q */
    long byte;
    int bit;   /* -1 for byte/word/... */
    int size;  /* bytes (0 for a bit) */
    char tag[200];
    char table[200];
    char type[64];
} tag_span;

typedef struct tag_spans {
    tag_span *v;
    int n, cap;
} tag_spans;

/* Parses %M10.3, %MB5, %MW20, %MD4, %IW64, %Q0.0, "%I0.0:P" (German E/A accepted). */
int ta_parse(const char *address, tag_span *s);
/* Size in bytes of an elementary type; *is_bit for Bool. -1 if unknown. */
int ta_type_size(const char *type, int *is_bit);
/* All tags with an M/I/Q address of a PLC, sorted by area and address.
   used_only keeps the tags referenced in the program (cross references). */
int ta_load(th plc_software, tag_spans *sp, int used_only);
int ta_overlaps(const tag_span *a, const tag_span *b);
void ta_free(tag_spans *sp);

#endif
