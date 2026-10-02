#include "config.h"

#include "cJSON.h"
#include "util/fs.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

tc_config g_cfg;

static void read_str(const cJSON *root, const char *key, char *dst, size_t cap)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, key));
    if (s)
        snprintf(dst, cap, "%s", s);
}

int config_load(void)
{
    char dir[TC_PATH_MAX];
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.log_level = LOG_INFO;
    if (fs_app_dir(FS_LOCAL, g_cfg.data_dir, sizeof g_cfg.data_dir) != 0)
        snprintf(g_cfg.data_dir, sizeof g_cfg.data_dir, ".");
    if (fs_app_dir(FS_ROAMING, dir, sizeof dir) != 0)
        snprintf(dir, sizeof dir, "%s", g_cfg.data_dir);
    fs_join(g_cfg.config_file, sizeof g_cfg.config_file, dir, "config.json");

    char *text = NULL;
    if (fs_read_all(g_cfg.config_file, &text, NULL) == 0) {
        cJSON *root = cJSON_Parse(text);
        if (root) {
            read_str(root, "projectsRoot", g_cfg.projects_root, sizeof g_cfg.projects_root);
            read_str(root, "archivesRoot", g_cfg.archives_root, sizeof g_cfg.archives_root);
            read_str(root, "exportsRoot", g_cfg.exports_root, sizeof g_cfg.exports_root);
            read_str(root, "librariesRoot", g_cfg.libraries_root, sizeof g_cfg.libraries_root);
            g_cfg.read_only = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "readOnly"));
            const cJSON *lvl = cJSON_GetObjectItemCaseSensitive(root, "logLevel");
            if (cJSON_IsString(lvl)) {
                if (_stricmp(lvl->valuestring, "debug") == 0)
                    g_cfg.log_level = LOG_DEBUG;
                else if (_stricmp(lvl->valuestring, "warn") == 0)
                    g_cfg.log_level = LOG_WARN;
                else if (_stricmp(lvl->valuestring, "error") == 0)
                    g_cfg.log_level = LOG_ERROR;
            }
            cJSON_Delete(root);
        } else {
            LOG_W("ignoring malformed %s", g_cfg.config_file);
        }
        free(text);
    }
    if (!g_cfg.exports_root[0])
        fs_join(g_cfg.exports_root, sizeof g_cfg.exports_root, g_cfg.data_dir, "exports");

    const char *ro = getenv("TIACMD_READONLY");
    if (ro && *ro && strcmp(ro, "0") != 0)
        g_cfg.read_only_forced = 1;
    return 0;
}

int config_read_only(void)
{
    return g_cfg.read_only || g_cfg.read_only_forced;
}

int config_save(void)
{
    static const char *levels[] = { "debug", "info", "warn", "error" };
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "projectsRoot", g_cfg.projects_root);
    cJSON_AddStringToObject(root, "archivesRoot", g_cfg.archives_root);
    cJSON_AddStringToObject(root, "exportsRoot", g_cfg.exports_root);
    cJSON_AddStringToObject(root, "librariesRoot", g_cfg.libraries_root);
    cJSON_AddBoolToObject(root, "readOnly", g_cfg.read_only);
    cJSON_AddStringToObject(root, "logLevel", levels[g_cfg.log_level & 3]);
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    int rc = text ? fs_write_all(g_cfg.config_file, text, strlen(text)) : -1;
    cJSON_free(text);
    return rc;
}
