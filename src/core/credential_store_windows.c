#include "credential_store.h"
#ifdef _WIN32
#include "win_private.h"
#include <wincrypt.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
/* DPAPI handles the full versioned payload (up to 64 KiB), unlike Credential
 * Manager's 2560-byte blob. No secret or key is ever written unencrypted. */
static void filename(const char *pk, char name[80]) { snprintf(name, 80, "%s.dpapi", pk); }
static int win_get(void *ctx, const char *pk, char **out) {
    char name[80]; filename(pk, name);
    HANDLE h = md_win_file(ctx, name, GENERIC_READ, OPEN_EXISTING, FILE_SHARE_READ);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? MD_CREDENTIAL_NOT_FOUND : MD_CREDENTIAL_FAILED;
    unsigned char *bytes = NULL; size_t size = 0;
    bool ok = md_win_read(h, &bytes, &size, 131072); CloseHandle(h);
    if (!ok) return MD_CREDENTIAL_FAILED;
    DATA_BLOB encrypted = {(DWORD)size, bytes}, plain = {0}, entropy = {64, (BYTE *)pk};
    int rc = MD_CREDENTIAL_LOCKED;
    if (CryptUnprotectData(&encrypted, NULL, &entropy, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
        rc = MD_CREDENTIAL_INVALID;
        if (plain.cbData && plain.cbData <= 65536 && !memchr(plain.pbData, 0, plain.cbData)) {
            *out = calloc((size_t)plain.cbData + 1, 1);
            if (*out) { memcpy(*out, plain.pbData, plain.cbData); rc = 0; }
            else rc = MD_CREDENTIAL_FAILED;
        }
    }
    if (plain.pbData) { SecureZeroMemory(plain.pbData, plain.cbData); LocalFree(plain.pbData); }
    md_credential_wipe(bytes, size); free(bytes); return rc;
}
static int win_put(void *ctx, const char *pk, const char *payload) {
    DATA_BLOB plain = {(DWORD)strlen(payload), (BYTE *)payload}, encrypted = {0}, entropy = {64, (BYTE *)pk};
    if (!CryptProtectData(&plain, L"Metadesk NIP-46", &entropy, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &encrypted))
        return MD_CREDENTIAL_LOCKED;
    char name[80]; filename(pk, name);
    bool ok = md_win_replace(ctx, name, encrypted.pbData, encrypted.cbData);
    SecureZeroMemory(encrypted.pbData, encrypted.cbData); LocalFree(encrypted.pbData);
    return ok ? 0 : MD_CREDENTIAL_FAILED;
}
static int win_remove(void *ctx, const char *pk) {
    char name[80]; filename(pk, name);
    return md_win_remove(ctx, name) ? 0 : MD_CREDENTIAL_FAILED;
}
static void win_destroy(void *ctx) { md_win_directory_close(ctx); }
MdCredentialStore *md_credential_store_windows(const char *directory) {
    char *path = directory ? NULL : md_win_default_directory("/metadesk/credentials");
    MdWinDirectory *d = md_win_directory(directory ? directory : path, true); free(path);
    if (!d) return NULL;
    static const MdCredentialStoreOps ops = {win_get, win_put, win_remove, win_destroy};
    MdCredentialStore *s = md_credential_store_custom(&ops, d);
    if (!s) md_win_directory_close(d);
    return s;
}
#else
MdCredentialStore *md_credential_store_windows(const char *directory) { (void)directory; return NULL; }
#endif
