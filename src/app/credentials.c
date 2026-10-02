#include "credentials.h"

#include "util/utf.h"

#include <windows.h>
#include <wincred.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREFIX L"tiaComandante/"

const char *cred_kind_name(cred_kind kind)
{
    return kind == CRED_UMAC ? "umac" : "plc";
}

int cred_kind_parse(const char *s, cred_kind *out)
{
    if (s && _stricmp(s, "umac") == 0) {
        *out = CRED_UMAC;
        return 0;
    }
    if (s && _stricmp(s, "plc") == 0) {
        *out = CRED_PLC;
        return 0;
    }
    return -1;
}

/* Keys are case-insensitive (paths and IPs): "<kind>/<key>" with the key in
   lower case and '\' as path separator. */
static wchar_t *target_name(cred_kind kind, const char *key)
{
    char norm[1024];
    snprintf(norm, sizeof norm, "%s", key && *key ? key : "*");
    for (char *p = norm; *p; p++) {
        if (*p == '/')
            *p = '\\';
        else if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p - 'A' + 'a');
    }
    char buf[1100];
    snprintf(buf, sizeof buf, "%s/%s", cred_kind_name(kind), norm);
    wchar_t *k = utf8_to_wide(buf);
    if (!k)
        return NULL;
    size_t n = wcslen(PREFIX) + wcslen(k) + 1;
    wchar_t *t = malloc(n * sizeof(wchar_t));
    if (t)
        _snwprintf_s(t, n, _TRUNCATE, L"%ls%ls", PREFIX, k);
    free(k);
    return t;
}

static int read_one(const wchar_t *target, cred *out)
{
    PCREDENTIALW c = NULL;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &c))
        return -1;
    memset(out, 0, sizeof *out);
    char *user = wide_to_utf8(c->UserName ? c->UserName : L"");
    snprintf(out->user, sizeof out->user, "%s", user ? user : "");
    free(user);
    /* The blob holds the UTF-16 password. */
    size_t wn = c->CredentialBlobSize / sizeof(wchar_t);
    wchar_t *wp = calloc(wn + 1, sizeof(wchar_t));
    if (wp) {
        memcpy(wp, c->CredentialBlob, wn * sizeof(wchar_t));
        out->password = wide_to_utf8(wp);
        SecureZeroMemory(wp, wn * sizeof(wchar_t));
        free(wp);
    }
    out->global = c->Comment && wcsstr(c->Comment, L"type=Global") != NULL;
    SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
    CredFree(c);
    return out->password ? 0 : -1;
}

int cred_get(cred_kind kind, const char *key, cred *out)
{
    memset(out, 0, sizeof *out);
    const char *keys[2] = { key, "*" };
    for (int i = 0; i < 2; i++) {
        if (!keys[i] || !*keys[i])
            continue;
        wchar_t *t = target_name(kind, keys[i]);
        int rc = t ? read_one(t, out) : -1;
        free(t);
        if (rc == 0)
            return 0;
    }
    return -1;
}

void cred_free(cred *c)
{
    if (c->password) {
        SecureZeroMemory(c->password, strlen(c->password));
        free(c->password);
    }
    memset(c, 0, sizeof *c);
}

int cred_prompt_store(cred_kind kind, const char *key, int global, const char *message, char *err, size_t errlen)
{
    wchar_t user[CREDUI_MAX_USERNAME_LENGTH + 1] = L"";
    wchar_t pass[CREDUI_MAX_PASSWORD_LENGTH + 1] = L"";
    cred existing;
    if (cred_get(kind, key, &existing) == 0) {
        wchar_t *u = utf8_to_wide(existing.user);
        if (u)
            wcsncpy_s(user, CREDUI_MAX_USERNAME_LENGTH + 1, u, _TRUNCATE);
        free(u);
        cred_free(&existing);
    }
    wchar_t *wmsg = utf8_to_wide(message ? message : "tiaComandante");
    CREDUI_INFOW info = { sizeof info };
    info.pszCaptionText = L"tiaComandante";
    info.pszMessageText = wmsg;
    BOOL save = FALSE;
    DWORD rc = CredUIPromptForCredentialsW(&info, L"tiaComandante", NULL, 0, user, CREDUI_MAX_USERNAME_LENGTH + 1, pass,
                                           CREDUI_MAX_PASSWORD_LENGTH + 1, &save,
                                           CREDUI_FLAGS_GENERIC_CREDENTIALS | CREDUI_FLAGS_ALWAYS_SHOW_UI |
                                               CREDUI_FLAGS_DO_NOT_PERSIST);
    free(wmsg);
    if (rc == ERROR_CANCELLED)
        return 1;
    if (rc != NO_ERROR) {
        snprintf(err, errlen, "credential dialog failed (error %lu)", rc);
        return -1;
    }
    wchar_t *target = target_name(kind, key);
    if (!target) {
        SecureZeroMemory(pass, sizeof pass);
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    CREDENTIALW c = { 0 };
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = target;
    c.UserName = user;
    c.CredentialBlob = (LPBYTE)pass;
    c.CredentialBlobSize = (DWORD)(wcslen(pass) * sizeof(wchar_t));
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    c.Comment = kind == CRED_UMAC ? (global ? L"type=Global" : L"type=Project")
                                  : (global ? L"PLC password / user, type=Global" : L"PLC password / user, type=Project");
    BOOL ok = CredWriteW(&c, 0);
    DWORD werr = GetLastError();
    SecureZeroMemory(pass, sizeof pass);
    free(target);
    if (!ok) {
        snprintf(err, errlen, "storing the credential failed (error %lu)", werr);
        return -1;
    }
    return 0;
}

int cred_delete(cred_kind kind, const char *key)
{
    wchar_t *t = target_name(kind, key);
    BOOL ok = t && CredDeleteW(t, CRED_TYPE_GENERIC, 0);
    free(t);
    return ok ? 0 : -1;
}

void cred_list(strbuf *out)
{
    DWORD n = 0;
    PCREDENTIALW *list = NULL;
    if (!CredEnumerateW(PREFIX L"*", 0, &n, &list)) {
        sb_append(out, "(no stored credentials)\n");
        return;
    }
    for (DWORD i = 0; i < n; i++) {
        char *t = wide_to_utf8(list[i]->TargetName + wcslen(PREFIX));
        char *u = wide_to_utf8(list[i]->UserName ? list[i]->UserName : L"");
        char *cm = wide_to_utf8(list[i]->Comment ? list[i]->Comment : L"");
        sb_printf(out, "%s  [user=%s%s%s]\n", t ? t : "?", u && *u ? u : "(none)", cm && *cm ? ", " : "", cm ? cm : "");
        free(t);
        free(u);
        free(cm);
    }
    CredFree(list);
}
