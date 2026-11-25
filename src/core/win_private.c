#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#endif
#include "win_private.h"
#ifdef _WIN32
#include <aclapi.h>
#include <sddl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

struct MdWinDirectory {
    wchar_t *path;
    HANDLE *parents; /* Pin every component against rename until close. */
    size_t count;
};
static TOKEN_USER *current_user(void) {
    HANDLE token = NULL;
    DWORD size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return NULL;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    TOKEN_USER *user = malloc(size);
    if (user && !GetTokenInformation(token, TokenUser, user, size, &size)) {
        free(user); user = NULL;
    }
    CloseHandle(token); return user;
}
static PSECURITY_DESCRIPTOR private_sd(void) {
    TOKEN_USER *user = current_user();
    LPWSTR sid = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (user && ConvertSidToStringSidW(user->User.Sid, &sid)) {
        wchar_t sddl[512];
        swprintf(sddl, 512, L"O:%lsD:P(A;OICI;FA;;;%ls)", sid, sid);
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL);
    }
    LocalFree(sid); free(user); return sd;
}
static bool private_handle(HANDLE h, bool directory) {
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory ||
        (!directory && info.nNumberOfLinks != 1)) return false;
    PSID owner = NULL; PACL acl = NULL; PSECURITY_DESCRIPTOR sd = NULL;
    TOKEN_USER *user = current_user();
    bool ok = user && GetSecurityInfo(h, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, NULL, &acl, NULL, &sd) == ERROR_SUCCESS &&
        owner && EqualSid(owner, user->User.Sid) && acl && IsValidAcl(acl);
    for (DWORD i = 0; ok && i < acl->AceCount; ++i) {
        ACE_HEADER *ace = NULL;
        if (!GetAce(acl, i, (void **)&ace)) { ok = false; break; }
        if (ace->AceFlags & INHERIT_ONLY_ACE) continue;
        if (ace->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        /* Reject unknown/object/callback grants rather than guessing effective
         * rights. Even Administrators and SYSTEM are not owner-only grants. */
        if (ace->AceType != ACCESS_ALLOWED_ACE_TYPE) { ok = false; break; }
        ACCESS_ALLOWED_ACE *allow = (ACCESS_ALLOWED_ACE *)ace;
        if (allow->Mask && !EqualSid(&allow->SidStart, user->User.Sid)) ok = false;
    }
    LocalFree(sd); free(user); return ok;
}
static wchar_t *native_path(const char *path) {
    if (!path) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (n < 4 || n > 32760) return NULL;
    wchar_t *p = calloc((size_t)n, sizeof(*p));
    if (!p || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, p, n)) { free(p); return NULL; }
    for (int i = 0; i < n; ++i) if (p[i] == L'/') p[i] = L'\\';
    /* Deliberately local drive-absolute paths only; no device/UNC namespaces. */
    if (!((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) ||
        p[1] != L':' || p[2] != L'\\' || wcschr(p + 2, L':')) { free(p); return NULL; }
    wchar_t drive[] = {p[0], L':', 0}, root[] = {p[0], L':', L'\\', 0};
    wchar_t device[512];
    /* Mapped network drives and SUBST aliases can hide an unchecked prefix. */
    if (GetDriveTypeW(root) != DRIVE_FIXED ||
        !QueryDosDeviceW(drive, device, 512) ||
        wcsncmp(device, L"\\Device\\HarddiskVolume", 22)) { free(p); return NULL; }
    for (wchar_t *part = p + 3; *part;) {
        wchar_t *end = wcschr(part, L'\\');
        size_t len = end ? (size_t)(end - part) : wcslen(part);
        if (!len || part[len-1] == L'.' || part[len-1] == L' ' ||
            wcscspn(part, L"*?\"<>|") < len) { free(p); return NULL; }
        part += len; if (*part) ++part;
    }
    size_t len = wcslen(p);
    if (len > 3 && p[len-1] == L'\\') p[len-1] = 0;
    return p;
}
void md_win_directory_close(MdWinDirectory *d) {
    if (!d) return;
    for (size_t i = d->count; i; --i) CloseHandle(d->parents[i-1]);
    free(d->parents); free(d->path); free(d);
}
MdWinDirectory *md_win_directory(const char *path, bool create) {
    MdWinDirectory *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->path = native_path(path);
    if (!d->path) goto failed;
    size_t len = wcslen(d->path);
    d->parents = calloc(len, sizeof(HANDLE));
    if (!d->parents) goto failed;
    PSECURITY_DESCRIPTOR sd = private_sd();
    if (!sd) goto failed;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
    /* Attribute-only opens do not participate in Win32 sharing checks.
     * FILE_LIST_DIRECTORY is essential to pin paths against rename/reparse. */
    for (size_t i = 3; i <= len; ++i) {
        if (d->path[i] && d->path[i] != L'\\') continue;
        wchar_t saved = d->path[i]; d->path[i] = 0;
        /* Only the private final directory may accept child updates. Its
         * owner-only DACL excludes untrusted reparse writers. Ancestors deny
         * write sharing, and every component denies delete sharing. */
        DWORD share = FILE_SHARE_READ | (i == len ? FILE_SHARE_WRITE : 0);
        HANDLE h = CreateFileW(d->path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
            share, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (h == INVALID_HANDLE_VALUE && create && GetLastError() == ERROR_FILE_NOT_FOUND) {
            if (CreateDirectoryW(d->path, &sa) || GetLastError() == ERROR_ALREADY_EXISTS)
                h = CreateFileW(d->path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
                    share, NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        }
        BY_HANDLE_FILE_INFORMATION info;
        bool ok = h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &info) &&
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            (i != len || private_handle(h, true));
        d->path[i] = saved;
        if (!ok) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); LocalFree(sd); goto failed; }
        d->parents[d->count++] = h;
    }
    LocalFree(sd); return d;
failed:
    md_win_directory_close(d); return NULL;
}
char *md_win_default_directory(const char *suffix) {
    wchar_t base[32768];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, 32768);
    if (!n || n >= 32768) return NULL;
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, base, -1, NULL, 0, NULL, NULL);
    if (!bytes) return NULL;
    char *path = malloc((size_t)bytes + strlen(suffix));
    if (path) { WideCharToMultiByte(CP_UTF8, 0, base, -1, path, bytes, NULL, NULL); strcat(path, suffix); }
    return path;
}
static wchar_t *child(MdWinDirectory *d, const char *name) {
    if (!d || !name || !*name || strpbrk(name, "/\\:")) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, NULL, 0);
    if (!n || name[strlen(name)-1] == '.' || name[strlen(name)-1] == ' ') return NULL;
    size_t len = wcslen(d->path);
    wchar_t *p = calloc(len + (size_t)n + 1, sizeof(*p));
    if (p) { wcscpy(p, d->path); p[len] = L'\\'; MultiByteToWideChar(CP_UTF8, 0, name, -1, p + len + 1, n); }
    return p;
}
static HANDLE open_private_file(MdWinDirectory *d, const char *name, DWORD access,
                                DWORD creation, DWORD share, DWORD flags) {
    wchar_t *path = child(d, name);
    PSECURITY_DESCRIPTOR sd = private_sd();
    if (!path || !sd) {
        free(path); LocalFree(sd); SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
    HANDLE h = CreateFileW(path, access | READ_CONTROL, share, &sa, creation,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS | flags, NULL);
    DWORD error = GetLastError();
    if (h != INVALID_HANDLE_VALUE && !private_handle(h, false)) {
        CloseHandle(h); h = INVALID_HANDLE_VALUE; error = ERROR_ACCESS_DENIED;
    }
    free(path); LocalFree(sd); SetLastError(error); return h;
}
HANDLE md_win_file(MdWinDirectory *d, const char *name, DWORD access, DWORD creation, DWORD share) {
    return open_private_file(d, name, access, creation, share, 0);
}
bool md_win_read(HANDLE h, unsigned char **out, size_t *size, size_t max) {
    *out = NULL; *size = 0;
    LARGE_INTEGER n; DWORD used = 0;
    if (!GetFileSizeEx(h, &n) || n.QuadPart <= 0 || n.QuadPart > MAXDWORD || (ULONGLONG)n.QuadPart > max) return false;
    unsigned char *data = calloc((size_t)n.QuadPart + 1, 1);
    if (!data) return false;
    bool ok = ReadFile(h, data, (DWORD)n.QuadPart, &used, NULL) && used == n.QuadPart;
    if (!ok) { SecureZeroMemory(data, (size_t)n.QuadPart); free(data); return false; }
    *out = data; *size = used; return true;
}
/* MoveFileEx cannot replace an open destination, even if it shares delete.
 * FileRenameInfoEx keeps the old snapshot readable on existing handles. */
static bool rename_snapshot(MdWinDirectory *d, const char *tmp, const wchar_t *dst) {
    size_t len = wcslen(dst) * sizeof(*dst);
    size_t bytes = sizeof(FILE_RENAME_INFO) + len;
    FILE_RENAME_INFO *info = calloc(1, bytes);
    if (!info) return false;
    info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->FileNameLength = (DWORD)len;
    memcpy(info->FileName, dst, len);
    /* FlushFileBuffers already handled the contents. Request write-through
     * for metadata as well; the rename handle must not deny new reads. */
    HANDLE h = open_private_file(d, tmp, DELETE, OPEN_EXISTING,
                                 FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_FLAG_WRITE_THROUGH);
    bool ok = h != INVALID_HANDLE_VALUE &&
        SetFileInformationByHandle(h, FileRenameInfoEx, info, (DWORD)bytes);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    free(info);
    return ok;
}
static bool replace_file(MdWinDirectory *d, const char *name, const void *data, size_t size,
                         bool snapshot) {
    static LONG serial;
    char tmp[80];
    snprintf(tmp, sizeof(tmp), ".write.%lu.%ld.tmp", GetCurrentProcessId(), InterlockedIncrement(&serial));
    HANDLE h = md_win_file(d, tmp, GENERIC_WRITE, CREATE_NEW, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = size <= MAXDWORD && WriteFile(h, data, (DWORD)size, &wrote, NULL) && wrote == size && FlushFileBuffers(h);
    CloseHandle(h);
    wchar_t *src = child(d, tmp), *dst = child(d, name);
    /* Validate an existing destination before replacing it. */
    HANDLE old = md_win_file(d, name, FILE_READ_ATTRIBUTES, OPEN_EXISTING, FILE_SHARE_READ);
    bool absent = old == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND;
    if (old != INVALID_HANDLE_VALUE) CloseHandle(old);
    ok = ok && (old != INVALID_HANDLE_VALUE || absent) && src && dst &&
        (snapshot ? rename_snapshot(d, tmp, dst) :
         MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    if (!ok && src) DeleteFileW(src);
    free(src); free(dst); return ok;
}
bool md_win_replace(MdWinDirectory *d, const char *name, const void *data, size_t size) {
    return replace_file(d, name, data, size, false);
}
bool md_win_replace_snapshot(MdWinDirectory *d, const char *name, const void *data, size_t size) {
    return replace_file(d, name, data, size, true);
}
bool md_win_remove(MdWinDirectory *d, const char *name) {
    HANDLE h = md_win_file(d, name, DELETE, OPEN_EXISTING, 0);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    FILE_DISPOSITION_INFO info = {TRUE};
    bool ok = SetFileInformationByHandle(h, FileDispositionInfo, &info, sizeof(info));
    CloseHandle(h); return ok;
}
bool md_win_read_path(const char *path, unsigned char **out, size_t *size, size_t max) {
    *out = NULL; *size = 0;
    char *copy = path ? strdup(path) : NULL;
    if (!copy) return false;
    char *slash = strrchr(copy, '/'), *back = strrchr(copy, '\\');
    if (!slash || (back && back > slash)) slash = back;
    if (!slash) { free(copy); return false; }
    *slash = 0;
    MdWinDirectory *d = md_win_directory(copy, false);
    HANDLE h = md_win_file(d, slash + 1, GENERIC_READ, OPEN_EXISTING, FILE_SHARE_READ);
    bool ok = h != INVALID_HANDLE_VALUE && md_win_read(h, out, size, max);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    md_win_directory_close(d); free(copy); return ok;
}
#endif
