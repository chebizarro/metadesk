/* Native owner-only storage. Handles are non-inheritable; reparse points,
 * alternate streams, hardlinks and grants to any other SID fail closed. */
#ifndef MD_WIN_PRIVATE_H
#define MD_WIN_PRIVATE_H
#ifdef _WIN32
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct MdWinDirectory MdWinDirectory;
MdWinDirectory *md_win_directory(const char *path, bool create);
void md_win_directory_close(MdWinDirectory *);
char *md_win_default_directory(const char *suffix);
HANDLE md_win_file(MdWinDirectory *, const char *name, DWORD access, DWORD creation, DWORD share);
bool md_win_read(HANDLE, unsigned char **out, size_t *size, size_t max);
bool md_win_replace(MdWinDirectory *, const char *name, const void *, size_t);
bool md_win_remove(MdWinDirectory *, const char *name);
bool md_win_read_path(const char *path, unsigned char **out, size_t *size, size_t max);
#endif
#endif
