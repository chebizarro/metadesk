/*
 * metadesk — log.c
 * Default sink and level filtering for log.h.
 */
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static MdLogSink  g_sink = NULL;
static void      *g_sink_ud = NULL;
static MdLogLevel g_level = MD_LOG_INFO;
static int        g_level_initialized = 0;

static void init_level_from_env(void) {
    if (g_level_initialized) return;
    g_level_initialized = 1;
    const char *v = getenv("MD_LOG_LEVEL");
    if (!v) return;
    if (strcmp(v, "error") == 0) g_level = MD_LOG_ERROR;
    else if (strcmp(v, "warn") == 0) g_level = MD_LOG_WARN;
    else if (strcmp(v, "info") == 0) g_level = MD_LOG_INFO;
    else if (strcmp(v, "debug") == 0) g_level = MD_LOG_DEBUG;
}

void md_log_set_sink(MdLogSink sink, void *userdata) {
    g_sink = sink;
    g_sink_ud = userdata;
}

void md_log_set_level(MdLogLevel level) {
    g_level_initialized = 1;
    g_level = level;
}

static void default_sink(MdLogLevel level, const char *tag,
                         const char *message, void *userdata) {
    (void)userdata;
    static const char *const names[] = { "ERROR", "WARN", "INFO", "DEBUG" };
    fprintf(stderr, "%s: %s: %s\n",
            names[level <= MD_LOG_DEBUG ? level : MD_LOG_INFO],
            tag ? tag : "?", message);
}

void md_log_emit(MdLogLevel level, const char *tag, const char *fmt, ...) {
    init_level_from_env();
    if (level > g_level) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (g_sink)
        g_sink(level, tag, buf, g_sink_ud);
    else
        default_sink(level, tag, buf, NULL);
}
