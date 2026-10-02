/* Credentials kept in the Windows Credential Manager (generic credentials,
   target names "tiaComandante/<kind>/<key>"). Passwords are entered only in the
   standard Windows credential dialog, never through tool arguments. */
#ifndef TC_CREDENTIALS_H
#define TC_CREDENTIALS_H

#include "util/strbuf.h"

#include <stddef.h>

typedef enum cred_kind {
    CRED_UMAC = 0, /* TIA project user management; key = project path or "*" */
    CRED_PLC = 1,  /* PLC access / protection password; key = PLC IP or "*" */
} cred_kind;

typedef struct cred {
    char user[256];
    char *password; /* malloc'ed, wiped by cred_free */
    int global;     /* UMAC: UmacUserType.Global instead of Project */
} cred;

/* Looks up key, then "*". Returns 0 when found. */
int cred_get(cred_kind kind, const char *key, cred *out);
void cred_free(cred *c);

/* Shows the Windows credential dialog and stores the result. Returns 0 when
   stored, 1 when cancelled by the user, -1 on error (message in err). */
int cred_prompt_store(cred_kind kind, const char *key, int global, const char *message, char *err, size_t errlen);
int cred_delete(cred_kind kind, const char *key);
/* One line per stored credential (never the secret). */
void cred_list(strbuf *out);

const char *cred_kind_name(cred_kind kind);
int cred_kind_parse(const char *s, cred_kind *out);

#endif
