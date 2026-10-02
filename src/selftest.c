/* --selftest: exercises the CLR host, the bridge and Openness V21 end to end
   (read-only, apart from exporting one block to %TEMP%). */
#include "selftest.h"

#include "tia/tia_dyn.h"
#include "util/utf.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
static volatile LONG g_confirmations;

static void check(int ok, const char *what)
{
    if (ok) {
        g_pass++;
        printf("  [ OK ] %s\n", what);
    } else {
        g_fail++;
        printf("  [FAIL] %s%s%s\n", what, td_failed() ? ": " : "", td_err());
    }
}

static int on_confirmation(void *ctx, const cJSON *args, cJSON **result)
{
    (void)ctx;
    (void)result;
    InterlockedIncrement(&g_confirmations);
    th e = tdv_h(cJSON_GetArrayItem(args, 1));
    char *caption = e ? td_get_s(e, "Caption") : NULL;
    char *text = e ? td_get_s(e, "Text") : NULL;
    printf("  [ EV ] Confirmation: %s / %s\n", caption ? caption : "", text ? text : "");
    free(caption);
    free(text);
    return 0;
}

static th find_plc_software(th items, int depth)
{
    if (depth > 6)
        return 0;
    cJSON *list = td_enum(items, "Name", -1);
    th found = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        th item = tdv_h(it);
        th sc = td_service(item, "Siemens.Engineering.HW.Features.SoftwareContainer");
        if (sc) {
            th sw = td_get_h(sc, "Software");
            if (td_is(sw, "Siemens.Engineering.SW.PlcSoftware")) {
                printf("         PlcSoftware found in DeviceItem '%s'\n", tdi_s(it, "Name"));
                found = sw;
                break;
            }
        }
        th sub = td_get_h(item, "DeviceItems");
        if (sub && (found = find_plc_software(sub, depth + 1)) != 0)
            break;
    }
    cJSON_Delete(list);
    return found;
}

static th first_block(th group)
{
    th blocks = td_get_h(group, "Blocks");
    cJSON *list = td_enum(blocks, "Name,Number,ProgrammingLanguage,IsConsistent", 15);
    th first = 0;
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, list)
    {
        char *num = tdv_text(tdi_a(it, "Number"));
        printf("         %-30s  [type=%s, num=%s, lang=%s, consistent=%s]\n", tdi_s(it, "Name"),
               strrchr(tdv_type(it), '.') ? strrchr(tdv_type(it), '.') + 1 : tdv_type(it), num,
               tdi_s(it, "ProgrammingLanguage") ? tdi_s(it, "ProgrammingLanguage") : "?",
               tdi_b(it, "IsConsistent", 0) ? "true" : "false");
        free(num);
        if (!first)
            first = tdv_h(it);
        n++;
    }
    cJSON_Delete(list);
    if (first)
        return first;
    th groups = td_get_h(group, "Groups");
    list = td_enum(groups, NULL, -1);
    cJSON_ArrayForEach(it, list)
    {
        if ((first = first_block(tdv_h(it))) != 0)
            break;
    }
    cJSON_Delete(list);
    return first;
}

int selftest_run(void)
{
    printf("tiaComandante selftest\n");

    cJSON *pong = td_request(cJSON_Parse("{\"op\":\"ping\"}"));
    check(pong && cJSON_IsString(pong) && strcmp(pong->valuestring, "pong") == 0, "bridge ping");
    cJSON_Delete(pong);

    cJSON *mem = td_members(0, "Siemens.Engineering.TiaPortal");
    check(mem != NULL, "resolve type Siemens.Engineering.TiaPortal (V21 assemblies)");
    cJSON_Delete(mem);

    cJSON *bad = td_static("System.Math", "NoSuchMethod", NULL);
    check(bad == NULL && strstr(td_err(), "NoSuchMethod") != NULL, "bridge error propagation");
    cJSON_Delete(bad);

    th procs = td_static_h("Siemens.Engineering.TiaPortal", "GetProcesses", NULL);
    check(procs != 0, "TiaPortal.GetProcesses()");
    cJSON *plist = td_enum(procs, "Id,Mode,ProjectPath", -1);
    int nproc = cJSON_GetArraySize(plist);
    printf("         %d TIA Portal V21 process(es)\n", nproc);
    th proc = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, plist)
    {
        const char *pp = tdi_s(it, "ProjectPath");
        printf("         pid=%lld mode=%s project=%s\n", tdi_i(it, "Id", 0), tdi_s(it, "Mode"), pp ? pp : "(none)");
        if (!proc || pp)
            proc = tdv_h(it);
    }
    cJSON_Delete(plist);
    if (!proc) {
        printf("  [SKIP] no TIA Portal V21 running: start TIA Portal V21 with a project to run the rest\n");
        goto summary;
    }

    printf("         attaching (TIA Portal may ask to allow access for this executable)...\n");
    th portal = td_call_h(proc, "Attach", NULL);
    check(portal != 0, "TiaPortalProcess.Attach()");
    if (!portal)
        goto summary;

    long long cb = td_register_callback(on_confirmation, NULL);
    long long sub = td_subscribe(portal, "Confirmation", cb);
    check(sub != 0, "subscribe TiaPortal.Confirmation (event -> C callback)");

    th projects = td_get_h(portal, "Projects");
    cJSON *prj = td_enum(projects, "Name,Path,IsModified", -1);
    check(prj != NULL, "enumerate Projects with attributes");
    th project = 0;
    cJSON_ArrayForEach(it, prj)
    {
        printf("         project '%s' path=%s modified=%s\n", tdi_s(it, "Name"), tdi_s(it, "Path"),
               tdi_b(it, "IsModified", 0) ? "yes" : "no");
        if (!project)
            project = tdv_h(it);
    }
    cJSON_Delete(prj);

    if (project) {
        th devices = td_get_h(project, "Devices");
        cJSON *dl = td_enum(devices, "Name,TypeIdentifier", -1);
        check(dl != NULL, "enumerate Devices");
        th plc = 0;
        cJSON_ArrayForEach(it, dl)
        {
            printf("         device '%s' (%s)\n", tdi_s(it, "Name"), tdi_s(it, "TypeIdentifier"));
            if (!plc)
                plc = find_plc_software(td_get_h(tdv_h(it), "DeviceItems"), 0);
        }
        cJSON_Delete(dl);
        check(plc != 0, "GetService<SoftwareContainer>() -> PlcSoftware");
        if (plc) {
            char *name = td_get_s(plc, "Name");
            printf("         PLC software '%s'\n", name ? name : "?");
            free(name);
            th block = first_block(td_get_h(plc, "BlockGroup"));
            check(block != 0, "enumerate blocks with Name/Number/ProgrammingLanguage");
            if (block) {
                wchar_t wtmp[MAX_PATH];
                GetTempPathW(MAX_PATH, wtmp);
                char *tmp = wide_to_utf8(wtmp);
                char path[MAX_PATH * 2];
                snprintf(path, sizeof path, "%stiacomandante_selftest_%lu.xml", tmp, GetCurrentProcessId());
                free(tmp);
                DeleteFileA(path);
                int rc = td_call_v(block, "Export", tda("fe", path, "Siemens.Engineering.ExportOptions", "WithDefaults"));
                check(rc == 0, "PlcBlock.Export(FileInfo, ExportOptions) with $file/$enum arguments");
                rc = td_call_v(block, "Export", tda("fs", path, "WithDefaults"));
                check(rc != 0 && td_err()[0] != 0, "Openness exception message reaches C (export to existing file)");
                printf("         -> %s: %s\n", td_err_type(), td_err());
                DeleteFileA(path);
            }
        }
    }

    td_unsubscribe(sub);
    td_unregister_callback(cb);
    check(td_call_v(portal, "Dispose", NULL) == 0, "TiaPortal.Dispose() (detach)");
    printf("         confirmations received: %ld, live handles: %lld\n", g_confirmations, td_handle_count());

summary:
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
