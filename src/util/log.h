#ifndef TC_LOG_H
#define TC_LOG_H

enum { LOG_DEBUG = 0, LOG_INFO = 1, LOG_WARN = 2, LOG_ERROR = 3 };

/* Opens the daily log file in dir (UTF-8 path, may be NULL for stderr only). */
void log_init(const char *dir);
void log_set_level(int level);
void log_set_stderr(int enabled);
void log_write(int level, const char *fmt, ...);
void log_close(void);

#define LOG_D(...) log_write(LOG_DEBUG, __VA_ARGS__)
#define LOG_I(...) log_write(LOG_INFO, __VA_ARGS__)
#define LOG_W(...) log_write(LOG_WARN, __VA_ARGS__)
#define LOG_E(...) log_write(LOG_ERROR, __VA_ARGS__)

#endif
