#ifndef TC_CONFIG_H
#define TC_CONFIG_H

#define TC_PATH_MAX 1024

/* config "confirmations": ask = leave the dialog to TIA Portal (the user answers),
   cancel = answer Cancel/No/Abort, accept = answer Yes/Ok. */
enum { CONF_ASK = 0, CONF_CANCEL = 1, CONF_ACCEPT = 2 };
extern const char *const config_confirmation_names[3];

typedef struct tc_config {
    char projects_root[TC_PATH_MAX];
    char archives_root[TC_PATH_MAX];
    char exports_root[TC_PATH_MAX];
    char libraries_root[TC_PATH_MAX];
    int read_only;        /* persisted setting */
    int read_only_forced; /* --read-only or TIACMD_READONLY */
    int log_level;
    int exclusive_access; /* ExclusiveAccess around project-modifying calls (default on) */
    int transactions;     /* ... and a transaction: one undo step, rollback on failure (default on) */
    int confirmations;    /* answer to TIA confirmation dialogs: CONF_ASK / CONF_CANCEL / CONF_ACCEPT */
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
