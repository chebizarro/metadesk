#include "md_compat.h"
#ifdef _WIN32
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
int md_setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=') || !value) { errno = EINVAL; return -1; }
    if (!overwrite && getenv(name)) return 0;
    int rc = _putenv_s(name, value);
    if (rc) { errno = rc; return -1; }
    return 0;
}
int md_unsetenv(const char *name) { return md_setenv(name, "", 1); }
int md_asprintf(char **out, const char *format, ...) {
    *out = NULL;
    va_list ap, copy;
    va_start(ap, format);
    va_copy(copy, ap);
    int len = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (len >= 0) {
        char *s = malloc((size_t)len + 1);
        if (s && vsnprintf(s, (size_t)len + 1, format, ap) == len) *out = s;
        else { free(s); len = -1; }
    }
    va_end(ap);
    return len;
}
char *md_strndup(const char *s, size_t n) {
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *out = malloc(len + 1);
    if (out) { memcpy(out, s, len); out[len] = 0; }
    return out;
}
ssize_t md_getline(char **line, size_t *capacity, FILE *file) {
    if (!line || !capacity || !file) { errno = EINVAL; return -1; }
    size_t len = 0;
    for (;;) {
        if (!*line || len + 1 >= *capacity) {
            size_t cap = *line && *capacity ? *capacity * 2 : 128;
            if (cap <= len || cap > (size_t)SSIZE_MAX) { errno = ENOMEM; return -1; }
            char *grown = realloc(*line, cap);
            if (!grown) return -1;
            *line = grown; *capacity = cap;
        }
        int c = fgetc(file);
        if (c == EOF) {
            if (ferror(file) || !len) return -1;
            break;
        }
        (*line)[len++] = (char)c;
        if (c == '\n') break;
    }
    (*line)[len] = 0;
    return (ssize_t)len;
}
#endif
