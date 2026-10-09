/* MCP over stdio: newline-delimited JSON-RPC 2.0.
 *
 * The main thread reads stdin and answers protocol requests directly
 * (initialize, ping, tools/list); tools/call is queued to a single worker
 * thread (MTA) that owns the CLR bridge and the TIA Portal connection, so
 * tool calls run one at a time and pings stay responsive during long
 * operations. The worker must not be an STA thread: Openness delivers TIA
 * Portal events to an STA client only while that thread is inside an
 * Openness call, and TIA Portal waits for a Confirmation answer, so an idle
 * STA worker froze TIA Portal at the first dialog of its user interface.
 * In an MTA client the events run on Openness threads.
 */
#include "server.h"

#include "app/config.h"
#include "mcp/registry.h"
#include "tc_version.h"
#include "tia/session.h"
#include "util/log.h"
#include "util/strbuf.h"

#include <windows.h>
#include <objbase.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_versions[] = { "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05" };

static const char *k_instructions =
    "tiaComandante drives Siemens TIA Portal V21 through the Openness API. Every tool takes an \"action\" "
    "argument (see each tool description). Typical workflow: session action=get_state, then session "
    "action=connect (attach to the running TIA Portal V21) or open/create; session action=list_devices gives "
    "the deviceName used by the PLC tools. Changes stay in the TIA project until session action=save. "
    "Operations that affect a real PLC require an explicit confirm phrase.";

static HANDLE g_out;
static CRITICAL_SECTION g_out_lock;
static cJSON *g_tools_cache;

typedef struct job {
    cJSON *id;
    char *tool;
    cJSON *args;
    cJSON *progress_token;
    volatile long cancelled;
    struct job *next;
} job;

static CRITICAL_SECTION g_q_lock;
static HANDLE g_q_event; /* auto-reset: a job was queued or stop was requested */
static job *g_head, *g_tail, *g_running;
static int g_stop;

/* ---- output ------------------------------------------------------------- */

static void write_all(const char *s, size_t n)
{
    while (n > 0) {
        DWORD put = 0;
        if (!WriteFile(g_out, s, (DWORD)(n > (1u << 30) ? (1u << 30) : n), &put, NULL) || put == 0)
            return;
        s += put;
        n -= put;
    }
}

static void send_json(cJSON *msg)
{
    char *s = cJSON_PrintUnformatted(msg);
    if (!s)
        return;
    EnterCriticalSection(&g_out_lock);
    write_all(s, strlen(s));
    write_all("\n", 1);
    LeaveCriticalSection(&g_out_lock);
    cJSON_free(s);
}

static cJSON *envelope(const cJSON *id)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "jsonrpc", "2.0");
    cJSON_AddItemToObject(m, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    return m;
}

static void send_result(const cJSON *id, cJSON *result)
{
    cJSON *m = envelope(id);
    cJSON_AddItemToObject(m, "result", result ? result : cJSON_CreateObject());
    send_json(m);
    cJSON_Delete(m);
}

static void send_error(const cJSON *id, int code, const char *message)
{
    cJSON *m = envelope(id);
    cJSON *e = cJSON_AddObjectToObject(m, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    send_json(m);
    cJSON_Delete(m);
}

static void send_progress(tool_ctx *c, double done, double total, const char *msg)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "jsonrpc", "2.0");
    cJSON_AddStringToObject(m, "method", "notifications/progress");
    cJSON *p = cJSON_AddObjectToObject(m, "params");
    cJSON_AddItemToObject(p, "progressToken", cJSON_Duplicate(c->progress_token, 1));
    cJSON_AddNumberToObject(p, "progress", done);
    if (total > 0)
        cJSON_AddNumberToObject(p, "total", total);
    if (msg)
        cJSON_AddStringToObject(p, "message", msg);
    send_json(m);
    cJSON_Delete(m);
}

static cJSON *call_result(tool_ctx *c)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(r, "content");
    cJSON *text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "type", "text");
    cJSON_AddStringToObject(text, "text", sb_str(&c->out));
    cJSON_AddItemToArray(content, text);
    cJSON_AddBoolToObject(r, "isError", c->is_error);
    return r;
}

/* ---- worker ---------------------------------------------------------------- */

static void free_job(job *j)
{
    cJSON_Delete(j->id);
    cJSON_Delete(j->args);
    cJSON_Delete(j->progress_token);
    free(j->tool);
    free(j);
}

static DWORD WINAPI worker_main(LPVOID arg)
{
    (void)arg;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr))
        LOG_E("worker CoInitializeEx failed (0x%08lx)", (unsigned long)hr);
    for (;;) {
        EnterCriticalSection(&g_q_lock);
        job *j = g_head;
        int stop = g_stop;
        if (j) {
            g_head = j->next;
            if (!g_head)
                g_tail = NULL;
            g_running = j;
        }
        LeaveCriticalSection(&g_q_lock);
        if (!j) {
            if (stop)
                break; /* stop requested and queue drained */
            WaitForSingleObject(g_q_event, INFINITE);
            continue;
        }

        if (!j->cancelled) {
            tool_ctx c;
            ctx_init(&c, NULL, j->args);
            c.progress_token = j->progress_token;
            c.progress_fn = send_progress;
            c.cancel = &j->cancelled;
            if (registry_execute(&c, j->tool) != 0) {
                char msg[256];
                snprintf(msg, sizeof msg, "Unknown tool: %s", j->tool);
                send_error(j->id, -32602, msg);
            } else if (!j->cancelled) {
                send_result(j->id, call_result(&c));
            }
            ctx_free(&c);
        }
        EnterCriticalSection(&g_q_lock);
        g_running = NULL;
        LeaveCriticalSection(&g_q_lock);
        free_job(j);
    }
    session_shutdown();
    CoUninitialize();
    return 0;
}

static void enqueue(job *j)
{
    EnterCriticalSection(&g_q_lock);
    if (g_tail)
        g_tail->next = j;
    else
        g_head = j;
    g_tail = j;
    LeaveCriticalSection(&g_q_lock);
    SetEvent(g_q_event);
}

static void cancel_request(const cJSON *request_id)
{
    if (!request_id)
        return;
    EnterCriticalSection(&g_q_lock);
    for (job *j = g_head; j; j = j->next)
        if (cJSON_Compare(j->id, request_id, 1))
            j->cancelled = 1;
    if (g_running && cJSON_Compare(g_running->id, request_id, 1))
        g_running->cancelled = 1;
    LeaveCriticalSection(&g_q_lock);
}

/* ---- protocol ------------------------------------------------------------- */

static cJSON *initialize_result(const cJSON *params)
{
    const char *wanted = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "protocolVersion"));
    const char *version = k_versions[0];
    for (size_t i = 0; wanted && i < sizeof k_versions / sizeof k_versions[0]; i++)
        if (strcmp(wanted, k_versions[i]) == 0)
            version = k_versions[i];
    const cJSON *client = cJSON_GetObjectItemCaseSensitive(params, "clientInfo");
    LOG_I("initialize: client %s %s, protocol %s -> %s",
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(client, "name")) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(client, "name")) : "?",
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(client, "version")) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(client, "version")) : "",
          wanted ? wanted : "?", version);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "protocolVersion", version);
    cJSON *caps = cJSON_AddObjectToObject(r, "capabilities");
    cJSON *tools = cJSON_AddObjectToObject(caps, "tools");
    cJSON_AddBoolToObject(tools, "listChanged", 0);
    cJSON *info = cJSON_AddObjectToObject(r, "serverInfo");
    cJSON_AddStringToObject(info, "name", TC_NAME);
    cJSON_AddStringToObject(info, "title", "tiaComandante (TIA Portal V21)");
    cJSON_AddStringToObject(info, "version", TC_VERSION);
    cJSON_AddStringToObject(r, "instructions", k_instructions);
    return r;
}

static void handle_message(const cJSON *msg)
{
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
    if (!cJSON_IsString(method)) {
        if (id && !cJSON_GetObjectItemCaseSensitive(msg, "result") && !cJSON_GetObjectItemCaseSensitive(msg, "error"))
            send_error(id, -32600, "Invalid Request");
        return; /* responses to server requests are not expected */
    }
    const char *m = method->valuestring;
    if (!id) {
        if (strcmp(m, "notifications/cancelled") == 0)
            cancel_request(cJSON_GetObjectItemCaseSensitive(params, "requestId"));
        return;
    }
    if (strcmp(m, "initialize") == 0) {
        send_result(id, initialize_result(params));
    } else if (strcmp(m, "ping") == 0) {
        send_result(id, NULL);
    } else if (strcmp(m, "tools/list") == 0) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "tools", cJSON_Duplicate(g_tools_cache, 1));
        send_result(id, r);
    } else if (strcmp(m, "tools/call") == 0) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params, "name"));
        if (!name) {
            send_error(id, -32602, "tools/call requires params.name");
            return;
        }
        if (!registry_find(name)) {
            char buf[256];
            snprintf(buf, sizeof buf, "Unknown tool: %s", name);
            send_error(id, -32602, buf);
            return;
        }
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        const cJSON *meta = cJSON_GetObjectItemCaseSensitive(params, "_meta");
        const cJSON *token = cJSON_GetObjectItemCaseSensitive(meta, "progressToken");
        job *j = calloc(1, sizeof *j);
        if (!j)
            return;
        j->id = cJSON_Duplicate(id, 1);
        j->tool = _strdup(name);
        j->args = cJSON_IsObject(args) ? cJSON_Duplicate(args, 1) : cJSON_CreateObject();
        j->progress_token = token ? cJSON_Duplicate(token, 1) : NULL;
        enqueue(j);
    } else if (strcmp(m, "resources/list") == 0) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddArrayToObject(r, "resources");
        send_result(id, r);
    } else if (strcmp(m, "resources/templates/list") == 0) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddArrayToObject(r, "resourceTemplates");
        send_result(id, r);
    } else if (strcmp(m, "prompts/list") == 0) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddArrayToObject(r, "prompts");
        send_result(id, r);
    } else {
        send_error(id, -32601, "Method not found");
    }
}

static void handle_line(char *line, size_t len)
{
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t'))
        line[--len] = 0;
    if (len == 0)
        return;
    cJSON *msg = cJSON_ParseWithLength(line, len);
    if (!msg) {
        send_error(NULL, -32700, "Parse error");
        return;
    }
    if (cJSON_IsArray(msg))
        send_error(NULL, -32600, "JSON-RPC batches are not supported");
    else if (!cJSON_IsObject(msg))
        send_error(NULL, -32600, "Invalid Request");
    else
        handle_message(msg);
    cJSON_Delete(msg);
}

/* Keeps stdout reserved for the protocol: anything else that writes to the
   console (CLR, Openness, CRT) is redirected to stderr. */
static int claim_stdout(void)
{
    HANDLE cur = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!DuplicateHandle(GetCurrentProcess(), cur, GetCurrentProcess(), &g_out, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return -1;
    SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
    fflush(stdout);
    _dup2(_fileno(stderr), _fileno(stdout));
    return 0;
}

int mcp_serve(void)
{
    char err[512];
    g_tools_cache = registry_tools_json(err, sizeof err);
    if (!g_tools_cache) {
        LOG_E("invalid tool table: %s", err);
        return 1;
    }
    if (claim_stdout() != 0) {
        LOG_E("cannot duplicate stdout");
        return 1;
    }
    InitializeCriticalSection(&g_out_lock);
    InitializeCriticalSection(&g_q_lock);
    g_q_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_q_event) {
        LOG_E("cannot create the job event");
        return 1;
    }
    LOG_I("tiacomandante %s MCP server started (%d tools, %d actions)%s", TC_VERSION, registry_count(),
          registry_action_total(), config_read_only() ? " [read-only]" : "");

    HANDLE worker = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    if (!worker) {
        LOG_E("cannot create worker thread");
        return 1;
    }

    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    strbuf buf;
    sb_init(&buf);
    char chunk[65536];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(in, chunk, sizeof chunk, &got, NULL) || got == 0)
            break;
        sb_appendn(&buf, chunk, got);
        size_t start = 0;
        for (size_t i = 0; i < buf.len; i++) {
            if (buf.p[i] == '\n') {
                buf.p[i] = 0;
                handle_line(buf.p + start, i - start);
                start = i + 1;
            }
        }
        if (start > 0) {
            memmove(buf.p, buf.p + start, buf.len - start);
            buf.len -= start;
            buf.p[buf.len] = 0;
        }
    }
    if (buf.len > 0)
        handle_line(buf.p, buf.len);
    sb_free(&buf);
    LOG_I("stdin closed, shutting down");

    EnterCriticalSection(&g_q_lock);
    g_stop = 1;
    LeaveCriticalSection(&g_q_lock);
    SetEvent(g_q_event);
    if (WaitForSingleObject(worker, 30000) == WAIT_TIMEOUT)
        LOG_W("worker still busy after 30 s, exiting anyway");
    CloseHandle(worker);
    cJSON_Delete(g_tools_cache);
    return 0;
}

int mcp_call_once(const char *tool, const char *json_args)
{
    cJSON *args = json_args && *json_args ? cJSON_Parse(json_args) : cJSON_CreateObject();
    if (!cJSON_IsObject(args)) {
        fprintf(stderr, "arguments must be a JSON object\n");
        cJSON_Delete(args);
        return 2;
    }
    tool_ctx c;
    ctx_init(&c, NULL, args);
    int rc;
    if (registry_execute(&c, tool) != 0) {
        fprintf(stderr, "unknown tool '%s'\n", tool);
        rc = 2;
    } else {
        fwrite(sb_str(&c.out), 1, c.out.len, stdout);
        rc = c.is_error ? 1 : 0;
    }
    ctx_free(&c);
    cJSON_Delete(args);
    session_shutdown();
    return rc;
}
