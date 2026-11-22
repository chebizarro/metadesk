#ifndef MD_COMPAT_H
#define MD_COMPAT_H
#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
/* CRT descriptors must not be inherited by spawned children. */
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
int md_setenv(const char *name, const char *value, int overwrite);
int md_unsetenv(const char *name);
#define setenv md_setenv
#define unsetenv md_unsetenv
int md_asprintf(char **out, const char *format, ...);
char *md_strndup(const char *s, size_t n);
ssize_t md_getline(char **line, size_t *capacity, FILE *file);
#define asprintf md_asprintf
#define strndup md_strndup
#define getline md_getline
#endif
#endif
