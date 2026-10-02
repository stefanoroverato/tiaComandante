/* Navigation helpers over the Openness object model. */
#ifndef TC_TIA_NAV_H
#define TC_TIA_NAV_H

#include "mcp/tool.h"
#include "tia/tia_dyn.h"

/* Called for each device (Devices + DeviceGroups, recursively). item is the
   enumeration entry carrying attributes Name and TypeIdentifier; group_path is
   "" for ungrouped devices. Return non-zero to stop. */
typedef int (*nav_device_fn)(void *ctx, th device, const cJSON *item, const char *group_path);
int nav_each_device(th project, nav_device_fn fn, void *ctx);

/* Called for each DeviceItem below device (depth-first, up to max_depth). */
typedef int (*nav_item_fn)(void *ctx, th item_h, const cJSON *item, int depth);
int nav_each_device_item(th device, int max_depth, nav_item_fn fn, void *ctx);

/* PlcSoftware of a DeviceItem, or 0. */
th nav_plc_software(th device_item);

/* First PlcSoftware below a device; optionally returns the CPU DeviceItem. */
th nav_device_plc(th device, th *cpu_item);

/* Resolves a TiaCommander "deviceName": device name, CPU DeviceItem name or
   PlcSoftware name (case-insensitive). Fails the call with the list of
   available PLCs when not found. */
typedef struct nav_plc {
    th device;
    th cpu;      /* DeviceItem hosting the software */
    th software; /* PlcSoftware */
    char device_name[256];
    char plc_name[256];
} nav_plc;
int nav_find_plc(tool_ctx *c, th project, const char *device_name, nav_plc *out);

/* Drops cached deviceName resolutions (project changed or objects disposed). */
void nav_cache_clear(void);

/* Online state of a CPU DeviceItem ("Offline", "Online", ...), or "" if no OnlineProvider. */
void nav_online_state(th cpu, char *out, size_t cap);

#endif
