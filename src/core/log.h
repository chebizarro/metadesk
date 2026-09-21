/*
 * metadesk — log.h
 * Minimal logging for core library code.
 *
 * Library modules must not write to stderr unbidden: the host may be a
 * GUI client or a daemon with its own log routing. Callers use the
 * MD_LOG_* macros with a per-module MD_LOG_TAG; the default sink prints
 * to stderr at MD_LOG_INFO and above, and a host can install its own
 * sink (GUI console, syslog, ...) and verbosity.
 *
 * Usage: #define MD_LOG_TAG "agent" at the top of the .c file, then
 *   MD_LOG_I("action=%s", name);
 */
#ifndef MD_LOG_H
#define MD_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MD_LOG_ERROR = 0,
    MD_LOG_WARN,
    MD_LOG_INFO,
    MD_LOG_DEBUG,
} MdLogLevel;

/* A sink receives one formatted line (level + tag + message). */
typedef void (*MdLogSink)(MdLogLevel level, const char *tag,
                          const char *message, void *userdata);

/* Install a custom sink (NULL restores the default stderr sink). */
void md_log_set_sink(MdLogSink sink, void *userdata);

/* Minimum level emitted (default: MD_LOG_INFO; MD_LOG_LEVEL env var
 * overrides at first emit: error/warn/info/debug). */
void md_log_set_level(MdLogLevel level);

void md_log_emit(MdLogLevel level, const char *tag, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#define MD_LOG_E(...) md_log_emit(MD_LOG_ERROR, MD_LOG_TAG, __VA_ARGS__)
#define MD_LOG_W(...) md_log_emit(MD_LOG_WARN,  MD_LOG_TAG, __VA_ARGS__)
#define MD_LOG_I(...) md_log_emit(MD_LOG_INFO,  MD_LOG_TAG, __VA_ARGS__)
#define MD_LOG_D(...) md_log_emit(MD_LOG_DEBUG, MD_LOG_TAG, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* MD_LOG_H */
