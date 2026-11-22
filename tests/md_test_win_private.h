#ifndef MD_TEST_WIN_PRIVATE_H
#define MD_TEST_WIN_PRIVATE_H
#ifdef _WIN32
#include "win_private.h"
#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static inline char *md_test_win_directory(void) {
    static LONG serial;
    char temp[32768], path[32768];
    DWORD n = GetTempPathA(sizeof(temp), temp);
    if (!n || n >= sizeof(temp)) return NULL;
    snprintf(path, sizeof(path), "%smd-w2-%lu-%ld", temp, GetCurrentProcessId(), InterlockedIncrement(&serial));
    MdWinDirectory *d = md_win_directory(path, true);
    if (!d) return NULL;
    md_win_directory_close(d); return strdup(path);
}
static inline bool md_test_win_acl(const char *path, bool private) {
    HANDLE token = NULL; DWORD n = 0; TOKEN_USER *user = NULL;
    LPSTR sid = NULL; PSECURITY_DESCRIPTOR sd = NULL; PACL acl = NULL; BOOL present, ignored;
    bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    if (ok) {
        GetTokenInformation(token, TokenUser, NULL, 0, &n); user = malloc(n);
        ok = user && GetTokenInformation(token, TokenUser, user, n, &n) && ConvertSidToStringSidA(user->User.Sid, &sid);
    }
    char sddl[512];
    if (ok) {
        snprintf(sddl, sizeof(sddl), "D:P(A;OICI;FA;;;%s)%s", sid, private ? "" : "(A;OICI;GR;;;WD)");
        ok = ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sd, NULL) &&
            GetSecurityDescriptorDacl(sd, &present, &acl, &ignored) && present &&
            SetNamedSecurityInfoA((char *)path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                NULL, NULL, acl, NULL) == ERROR_SUCCESS;
    }
    if (token) CloseHandle(token);
    LocalFree(sid); LocalFree(sd); free(user); return ok;
}
#endif
#endif
