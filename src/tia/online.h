/* Online access helpers: providers, configured IP, PG/PC interfaces. */
#ifndef TC_ONLINE_H
#define TC_ONLINE_H

#include "app/credentials.h"
#include "mcp/tool.h"
#include "tia/tia_dyn.h"
#include "tia/tia_nav.h"
#include "util/strbuf.h"

th on_online_provider(const nav_plc *plc);
th on_download_provider(const nav_plc *plc);

/* PROFINET/Ethernet nodes of the CPU: first IP in ip (may be empty) and one
   line per interface in details (may be NULL). */
void on_device_ip(const nav_plc *plc, char *ip, size_t cap, strbuf *details);

/* Finds the target interface of a ConnectionConfiguration: mode (default
   "PN/IE"), PG/PC interface by (partial, case-insensitive) name and optional
   target interface name. Fails the call listing what is available. */
th on_find_target(tool_ctx *c, th configuration, const char *mode, const char *pc_interface, const char *target,
                  th *pc_out);
void on_describe_interfaces(tool_ctx *c, th configuration);

/* Applies the optional legacyCommunication argument to a ConnectionConfiguration
   (EnableLegacyCommunication: non-secure PG/PC communication, for CPUs that allow
   it) for one operation. what names the operation in the output. *prev receives
   the previous value for on_restore_legacy (-1 = not changed). Returns -1 on
   failure. The setting is stored in the project and shared by the online and
   download configurations; left on, TIA drops the configured connection when
   going offline, so it must be restored after the operation. */
int on_apply_legacy(tool_ctx *c, th configuration, const char *what, int *prev);
void on_restore_legacy(th configuration, int prev);
/* After a failed connection: suggests legacyCommunication=true when it was not used. */
void on_legacy_hint(tool_ctx *c);

#define PLC_PASSWORD_ARGS                                                                                              \
    "passwords are not accepted as tool arguments (they would pass through the chat): store the PLC password once "   \
    "with admin action=set_credential kind=plc key=<PLC IP> (Windows dialog); it is then used automatically"

/* PLC access passwords / PLC user credentials for one connection, read lazily
   from the Windows Credential Manager (kind plc, key = PLC IP, then "*") when
   TIA Portal asks for them. */
typedef struct on_secret {
    char ip[64];
    int looked_up;
    int found;
    int unanswered; /* requests left without an answer */
    cred cr;
    char note[256];
} on_secret;
void on_secret_init(on_secret *s, const char *ip);
void on_secret_free(on_secret *s);
/* Answers a password request of a download/upload/online configuration object:
   -1 not a password request, 0 answered (note says how), 1 not answered (note says why). */
int on_answer_password(on_secret *s, th configuration, const char **note);

/* Answers the OnlineLegitimation event of a ConnectionConfiguration while going
   online. on_legitimation_begin returns the subscription (0 if unavailable). */
typedef struct on_legit {
    on_secret secret;
    strbuf log;
    long long sub;
    int trust_certificate; /* answer TLS certificate verification with Trusted (argument trustPlcCertificate) */
} on_legit;
void on_legitimation_begin(on_legit *l, th configuration, const char *ip);
/* Prints the answered requests; returns how many were left unanswered. */
int on_legitimation_end(tool_ctx *c, on_legit *l);

#endif
