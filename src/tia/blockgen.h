/* Block/UDT generation: SimaticML templates (imported with Openness) and
   SCL external sources (compiled with GenerateBlocksFromSource). */
#ifndef TC_BLOCKGEN_H
#define TC_BLOCKGEN_H

#include "mcp/tool.h"
#include "mxml.h"
#include "tia/tia_dyn.h"

typedef struct bg_member {
    char section[32];
    char name[128];
    char type[256];
    char start[256];
    char comment[512];
} bg_member;

typedef struct bg_spec {
    const char *kind;           /* FB, FC, OB, GlobalDB, PlcStruct */
    const char *name;
    long long number;           /* 0 = automatic */
    const char *language;       /* SCL, LAD, FBD, DB */
    const char *author, *family, *version;
    const char *title, *comment;
    const char *culture;        /* language of texts, e.g. en-US */
    const char *memory_layout;  /* Optimized (default) or Standard */
    const char *secondary_type; /* OB: ProgramCycle (default) */
    const char *return_type;    /* FC: Void (default) */
    const bg_member *members;
    int nmembers;
    int networks;               /* LAD/FBD: number of empty networks (default 1) */
    const char *const *network_titles;
} bg_spec;

/* Complete SimaticML document for an empty block / UDT with the given interface. */
mxml_node_t *bg_build_xml(const bg_spec *s);
/* SCL source for FB/FC/OB (code = statement part), GlobalDB or PlcStruct. Caller frees. */
char *bg_build_scl(const bg_spec *s, const char *code);

/* Parses the "interface" argument: {"Input":[{name,dataType,startValue,comment}],...}
   or [{section,name,dataType,...}]. Data types naming PLC data types or FBs are
   quoted. default_section is used for entries without a section. */
int bg_parse_interface(tool_ctx *c, const cJSON *iface, th plc_software, const char *default_section, bg_member **out,
                       int *n);

#endif
