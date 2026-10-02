/* CLR v4 hosting through mscoree.dll. The COM interfaces are declared here
   because metahost.h ships only with the .NET Framework SDK. */
#include "clrhost.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef HRESULT (WINAPI *CLRCreateInstanceFn)(REFCLSID clsid, REFIID riid, LPVOID *ppInterface);

static const GUID CLSID_CLRMetaHost_ = { 0x9280188d, 0x0e8e, 0x4867, { 0xb3, 0x0c, 0x7f, 0xa8, 0x38, 0x84, 0xe8, 0xde } };
static const GUID IID_ICLRMetaHost_ = { 0xd332db9e, 0xb9b3, 0x4125, { 0x82, 0x07, 0xa1, 0x48, 0x84, 0xf5, 0x32, 0x16 } };
static const GUID IID_ICLRRuntimeInfo_ = { 0xbd39d1d2, 0xba2f, 0x486a, { 0x89, 0xb0, 0xb4, 0xb0, 0xcb, 0x46, 0x68, 0x91 } };
static const GUID CLSID_CLRRuntimeHost_ = { 0x90f1a06e, 0x7712, 0x4762, { 0x86, 0xb5, 0x7a, 0x5e, 0xba, 0x6b, 0xdb, 0x02 } };
static const GUID IID_ICLRRuntimeHost_ = { 0x90f1a06c, 0x7712, 0x4762, { 0x86, 0xb5, 0x7a, 0x5e, 0xba, 0x6b, 0xdb, 0x02 } };

typedef struct ICLRMetaHost_ ICLRMetaHost_;
typedef struct ICLRMetaHostVtbl_ {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICLRMetaHost_ *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(ICLRMetaHost_ *);
    ULONG (STDMETHODCALLTYPE *Release)(ICLRMetaHost_ *);
    HRESULT (STDMETHODCALLTYPE *GetRuntime)(ICLRMetaHost_ *, LPCWSTR, REFIID, LPVOID *);
    void *GetVersionFromFile;
    void *EnumerateInstalledRuntimes;
    void *EnumerateLoadedRuntimes;
    void *RequestRuntimeLoadedNotification;
    void *QueryLegacyV2RuntimeBinding;
    void *ExitProcess;
} ICLRMetaHostVtbl_;
struct ICLRMetaHost_ { const ICLRMetaHostVtbl_ *lpVtbl; };

typedef struct ICLRRuntimeInfo_ ICLRRuntimeInfo_;
typedef struct ICLRRuntimeInfoVtbl_ {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICLRRuntimeInfo_ *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(ICLRRuntimeInfo_ *);
    ULONG (STDMETHODCALLTYPE *Release)(ICLRRuntimeInfo_ *);
    void *GetVersionString;
    void *GetRuntimeDirectory;
    void *IsLoaded;
    void *LoadErrorString;
    void *LoadLibrary;
    void *GetProcAddress;
    HRESULT (STDMETHODCALLTYPE *GetInterface)(ICLRRuntimeInfo_ *, REFCLSID, REFIID, LPVOID *);
    HRESULT (STDMETHODCALLTYPE *IsLoadable)(ICLRRuntimeInfo_ *, BOOL *);
    void *SetDefaultStartupFlags;
    void *GetDefaultStartupFlags;
    void *BindAsLegacyV2Runtime;
    void *IsStarted;
} ICLRRuntimeInfoVtbl_;
struct ICLRRuntimeInfo_ { const ICLRRuntimeInfoVtbl_ *lpVtbl; };

typedef struct ICLRRuntimeHost_ ICLRRuntimeHost_;
typedef struct ICLRRuntimeHostVtbl_ {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICLRRuntimeHost_ *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(ICLRRuntimeHost_ *);
    ULONG (STDMETHODCALLTYPE *Release)(ICLRRuntimeHost_ *);
    HRESULT (STDMETHODCALLTYPE *Start)(ICLRRuntimeHost_ *);
    HRESULT (STDMETHODCALLTYPE *Stop)(ICLRRuntimeHost_ *);
    void *SetHostControl;
    void *GetCLRControl;
    void *UnloadAppDomain;
    void *ExecuteInAppDomain;
    void *GetCurrentAppDomainId;
    void *ExecuteApplication;
    HRESULT (STDMETHODCALLTYPE *ExecuteInDefaultAppDomain)(ICLRRuntimeHost_ *, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, DWORD *);
} ICLRRuntimeHostVtbl_;
struct ICLRRuntimeHost_ { const ICLRRuntimeHostVtbl_ *lpVtbl; };

int clr_start_bridge(const wchar_t *bridge_dll, const wchar_t *openness_dir,
                     tc_native_cb cb, tc_bridge_api *api, char *err, size_t errlen)
{
    ICLRMetaHost_ *meta = NULL;
    ICLRRuntimeInfo_ *info = NULL;
    ICLRRuntimeHost_ *host = NULL;
    HRESULT hr;
    int rc = -1;

    memset(api, 0, sizeof *api);
    api->size = (uint32_t)sizeof *api;

    HMODULE mscoree = LoadLibraryW(L"mscoree.dll");
    if (!mscoree) {
        snprintf(err, errlen, "mscoree.dll not found: .NET Framework 4.8 is required");
        return -1;
    }
    CLRCreateInstanceFn create = (CLRCreateInstanceFn)(void *)GetProcAddress(mscoree, "CLRCreateInstance");
    if (!create) {
        snprintf(err, errlen, "CLRCreateInstance not exported by mscoree.dll");
        return -1;
    }
    hr = create(&CLSID_CLRMetaHost_, &IID_ICLRMetaHost_, (LPVOID *)&meta);
    if (FAILED(hr)) {
        snprintf(err, errlen, "CLRCreateInstance failed (0x%08lx)", (unsigned long)hr);
        goto done;
    }
    hr = meta->lpVtbl->GetRuntime(meta, L"v4.0.30319", &IID_ICLRRuntimeInfo_, (LPVOID *)&info);
    if (FAILED(hr)) {
        snprintf(err, errlen, "CLR v4.0.30319 not available (0x%08lx)", (unsigned long)hr);
        goto done;
    }
    BOOL loadable = FALSE;
    hr = info->lpVtbl->IsLoadable(info, &loadable);
    if (FAILED(hr) || !loadable) {
        snprintf(err, errlen, "CLR v4 is not loadable in this process (0x%08lx)", (unsigned long)hr);
        goto done;
    }
    hr = info->lpVtbl->GetInterface(info, &CLSID_CLRRuntimeHost_, &IID_ICLRRuntimeHost_, (LPVOID *)&host);
    if (FAILED(hr)) {
        snprintf(err, errlen, "ICLRRuntimeHost unavailable (0x%08lx)", (unsigned long)hr);
        goto done;
    }
    hr = host->lpVtbl->Start(host);
    if (FAILED(hr)) {
        snprintf(err, errlen, "CLR start failed (0x%08lx)", (unsigned long)hr);
        goto done;
    }

    wchar_t arg[2048];
    _snwprintf_s(arg, sizeof arg / sizeof arg[0], _TRUNCATE, L"api=%llx\ncb=%llx\ndir=%ls",
                 (unsigned long long)(uintptr_t)api, (unsigned long long)(uintptr_t)cb,
                 openness_dir ? openness_dir : L"");
    DWORD ret = 0;
    hr = host->lpVtbl->ExecuteInDefaultAppDomain(host, bridge_dll, L"TiaComandante.Bridge.Entry", L"Init", arg, &ret);
    if (FAILED(hr)) {
        snprintf(err, errlen, "loading bridge %ls failed (0x%08lx)", bridge_dll, (unsigned long)hr);
        goto done;
    }
    if (ret != 0 || !api->invoke || !api->free_str) {
        snprintf(err, errlen, "bridge initialisation failed (code %lu)", (unsigned long)ret);
        goto done;
    }
    rc = 0;

done:
    /* The runtime stays loaded for the life of the process; only the COM
       references are dropped here. */
    if (host)
        host->lpVtbl->Release(host);
    if (info)
        info->lpVtbl->Release(info);
    if (meta)
        meta->lpVtbl->Release(meta);
    return rc;
}
