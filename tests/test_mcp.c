/* Protocol smoke test: starts tiacomandante.exe on pipes and checks the MCP
   handshake, tools/list schemas and error handling. Needs no TIA Portal. */
#include "cJSON.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            printf("  ok   ");                                                                                         \
        } else {                                                                                                       \
            printf("  FAIL ");                                                                                         \
            g_fail++;                                                                                                  \
        }                                                                                                              \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
    } while (0)

static HANDLE g_in_w, g_out_r;
static char g_buf[1 << 22];
static size_t g_len;

static void send_line(const char *s)
{
    DWORD put;
    WriteFile(g_in_w, s, (DWORD)strlen(s), &put, NULL);
    WriteFile(g_in_w, "\n", 1, &put, NULL);
}

/* Reads one response line (30 s timeout). Caller frees. */
static cJSON *read_msg(void)
{
    ULONGLONG deadline = GetTickCount64() + 30000;
    for (;;) {
        char *nl = memchr(g_buf, '\n', g_len);
        if (nl) {
            *nl = 0;
            cJSON *m = cJSON_Parse(g_buf);
            size_t used = (size_t)(nl - g_buf) + 1;
            memmove(g_buf, g_buf + used, g_len - used);
            g_len -= used;
            return m;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(g_out_r, NULL, 0, NULL, &avail, NULL))
            return NULL;
        if (avail == 0) {
            if (GetTickCount64() > deadline)
                return NULL;
            Sleep(10);
            continue;
        }
        DWORD got = 0;
        if (!ReadFile(g_out_r, g_buf + g_len, (DWORD)(sizeof g_buf - g_len - 1), &got, NULL) || got == 0)
            return NULL;
        g_len += got;
    }
}

static cJSON *request(const char *line)
{
    send_line(line);
    return read_msg();
}

static int has_error_code(const cJSON *m, int code)
{
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(m, "error");
    return e && (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(e, "code")) == code;
}

static void check_tools(const cJSON *tools)
{
    int n = cJSON_GetArraySize(tools);
    CHECK(n >= 3, "tools/list returns %d tools", n);
    const cJSON *t;
    cJSON_ArrayForEach(t, tools)
    {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(t, "name"));
        const cJSON *schema = cJSON_GetObjectItemCaseSensitive(t, "inputSchema");
        const cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(schema, "type"));
        int ok = name && type && strcmp(type, "object") == 0 && cJSON_IsObject(props) &&
                 cJSON_IsString(cJSON_GetObjectItemCaseSensitive(t, "description"));
        const cJSON *action = cJSON_GetObjectItemCaseSensitive(props, "action");
        if (action) {
            const cJSON *en = cJSON_GetObjectItemCaseSensitive(action, "enum");
            ok = ok && cJSON_GetArraySize(en) > 0 &&
                 cJSON_IsString(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(schema, "required"), 0));
        }
        const cJSON *p;
        cJSON_ArrayForEach(p, props)
        {
            if (!cJSON_GetObjectItemCaseSensitive(p, "type"))
                ok = 0;
        }
        CHECK(ok, "tool '%s' has a valid schema (%d properties)", name ? name : "?", cJSON_GetArraySize(props));
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_mcp path\\to\\tiacomandante.exe\n");
        return 2;
    }
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_r, out_w;
    if (!CreatePipe(&in_r, &g_in_w, &sa, 0) || !CreatePipe(&g_out_r, &out_w, &sa, 0))
        return 2;
    SetHandleInformation(g_in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(g_out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "\"%s\" --log-level warn", argv[1]);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "cannot start %s\n", argv[1]);
        return 2;
    }
    CloseHandle(in_r);
    CloseHandle(out_w);

    printf("MCP protocol smoke test\n");
    cJSON *m = request("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\","
                       "\"capabilities\":{},\"clientInfo\":{\"name\":\"test_mcp\",\"version\":\"1\"}}}");
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "result");
    CHECK(r != NULL, "initialize answers");
    CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "protocolVersion")) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "protocolVersion")) : "", "2025-06-18") == 0,
          "protocol version negotiated");
    CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "capabilities"), "tools") != NULL,
          "tools capability advertised");
    cJSON_Delete(m);

    m = request("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"1999-01-01\"}}");
    r = cJSON_GetObjectItemCaseSensitive(m, "result");
    CHECK(r && cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "protocolVersion")), "unknown protocol version falls back to a supported one");
    cJSON_Delete(m);

    send_line("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

    m = request("{\"jsonrpc\":\"2.0\",\"id\":\"p\",\"method\":\"ping\"}");
    CHECK(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(m, "result")) &&
              strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "id")), "p") == 0,
          "ping with string id");
    cJSON_Delete(m);

    m = request("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}");
    check_tools(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(m, "result"), "tools"));
    cJSON_Delete(m);

    m = request("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"get_info\",\"arguments\":{}}}");
    r = cJSON_GetObjectItemCaseSensitive(m, "result");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(r, "content"), 0), "text");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(r, "isError")) && cJSON_IsString(text) &&
              strstr(text->valuestring, "tiaComandante"),
          "tools/call get_info");
    cJSON_Delete(m);

    m = request("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"session\",\"arguments\":{\"action\":\"nope\"}}}");
    r = cJSON_GetObjectItemCaseSensitive(m, "result");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "isError")), "unknown action -> isError result");
    cJSON_Delete(m);

    m = request("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"no_such_tool\"}}");
    CHECK(has_error_code(m, -32602), "unknown tool -> -32602");
    cJSON_Delete(m);

    m = request("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"no/such\"}");
    CHECK(has_error_code(m, -32601), "unknown method -> -32601");
    cJSON_Delete(m);

    m = request("{this is not json");
    CHECK(has_error_code(m, -32700), "malformed JSON -> -32700");
    cJSON_Delete(m);

    m = request("[{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"ping\"}]");
    CHECK(has_error_code(m, -32600), "batch -> -32600");
    cJSON_Delete(m);

    m = request("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"admin\",\"arguments\":{\"action\":\"get_version\"}}}");
    r = cJSON_GetObjectItemCaseSensitive(m, "result");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(r, "isError")), "admin get_version");
    cJSON_Delete(m);

    CloseHandle(g_in_w);
    DWORD wr = WaitForSingleObject(pi.hProcess, 40000);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    CHECK(wr == WAIT_OBJECT_0 && code == 0, "server exits cleanly when stdin closes (code %lu)", code);
    if (wr != WAIT_OBJECT_0)
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
