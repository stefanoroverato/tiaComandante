/* Online access helpers: providers, configured IP, PG/PC interfaces. */
#ifndef TC_ONLINE_H
#define TC_ONLINE_H

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

#endif
