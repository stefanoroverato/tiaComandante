#ifndef TC_CLRHOST_H
#define TC_CLRHOST_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* Function table filled by TiaComandante.Bridge.Entry.Init (layout shared with Bridge.cs). */
typedef struct tc_bridge_api {
    uint32_t size;
    uint32_t version;
    int (__cdecl *invoke)(const char *req, char **resp);
    void (__cdecl *free_str)(char *p);
} tc_bridge_api;

/* Native callback invoked by the bridge for events/delegates. *result must be
   NULL or a CoTaskMemAlloc'ed UTF-8 JSON string. */
typedef int (__cdecl *tc_native_cb)(long long id, const char *args, char **result);

/* Starts CLR v4 in-process, loads the bridge assembly and fills api.
   Returns 0 on success, otherwise writes a message to err. */
int clr_start_bridge(const wchar_t *bridge_dll, const wchar_t *openness_dir,
                     tc_native_cb cb, tc_bridge_api *api, char *err, size_t errlen);

#endif
