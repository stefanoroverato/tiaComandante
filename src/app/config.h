#ifndef TC_CONFIG_H
#define TC_CONFIG_H

#define TC_PATH_MAX 1024

typedef struct tc_config {
    char projects_root[TC_PATH_MAX];
    char archives_root[TC_PATH_MAX];
    char exports_root[TC_PATH_MAX];
    char libraries_root[TC_PATH_MAX];
    int read_only;        /* persisted setting */
    int read_only_forced; /* --read-only or TIACMD_READONLY */
    int log_level;
    /* Derived, not persisted. */
    char config_file[TC_PATH_MAX];
    char data_dir[TC_PATH_MAX]; /* %LOCALAPPDATA%\tiaComandante */
} tc_config;

extern tc_config g_cfg;

/* Loads %APPDATA%\tiaComandante\config.json (missing file = defaults) and
   applies environment overrides (TIACMD_READONLY). */
int config_load(void);
int config_save(void);
int config_read_only(void);

#endif
