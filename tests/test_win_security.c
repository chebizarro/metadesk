/* Native-only security gates: no mode-bit emulation, mock crypto or keyring. */
#include "accounts.h"
#include "md_test.h"
#include "md_test_win_private.h"
#include <wincrypt.h>
#include <unistd.h>

static const char *pk = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static const char *other_pk = "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5";
static const char *secret = "0000000000000000000000000000000000000000000000000000000000000002";
static void test_native_files(void) {
    char *dir = md_test_win_directory(); MD_CHECK(dir != NULL); if (!dir) return;
    MdWinDirectory *d = md_win_directory(dir, false); MD_CHECK(d != NULL);
    char path[32768], link[32768], renamed[32768];
    snprintf(path, sizeof(path), "%s/token", dir);
    snprintf(link, sizeof(link), "%s/link", dir);
    snprintf(renamed, sizeof(renamed), "%s-renamed", dir);
    MD_CHECK(md_win_replace(d, "token", "test-secret\r\n", 13));
    unsigned char *bytes = NULL; size_t size = 0;
    MD_CHECK(md_win_read_path(path, &bytes, &size, 4079) && size == 13); free(bytes);
    MD_CHECK(!MoveFileA(dir, renamed)); /* pinned ancestors prevent substitution */
    HANDLE h = md_win_file(d, "token", GENERIC_READ, OPEN_EXISTING, FILE_SHARE_READ);
    MD_CHECK(h != INVALID_HANDLE_VALUE);
    HANDLE writer = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    MD_CHECK(writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    DWORD flags = HANDLE_FLAG_INHERIT; MD_CHECK(GetHandleInformation(h, &flags) && !(flags & HANDLE_FLAG_INHERIT));
    CloseHandle(h);
    MD_CHECK(md_test_win_acl(path, false));
    MD_CHECK(!md_win_read_path(path, &bytes, &size, 4079) && !bytes);
    MD_CHECK(md_test_win_acl(path, true));
    MD_CHECK(SetNamedSecurityInfoA(path, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
    MD_CHECK(!md_win_read_path(path, &bytes, &size, 4079));
    MD_CHECK(md_test_win_acl(path, true));
    MD_CHECK(CreateHardLinkA(link, path, NULL));
    MD_CHECK(!md_win_read_path(path, &bytes, &size, 4079));
    MD_CHECK(DeleteFileA(link));
    MD_CHECK(!md_win_read_path("relative/token", &bytes, &size, 4079));
    char stream[32768]; snprintf(stream, sizeof(stream), "%s:secret", path);
    MD_CHECK(!md_win_read_path(stream, &bytes, &size, 4079));
    MD_CHECK(!md_win_directory("C:/Users/../Users", false));
    MD_CHECK(!md_win_directory("C:/", false));
    MD_CHECK(!md_win_directory("C:", false));
    MD_CHECK(md_win_replace(d, "token", "large", 5));
    MD_CHECK(!md_win_read_path(path, &bytes, &size, 4));
    MD_CHECK(md_win_remove(d, "token"));
    MD_CHECK(CreateDirectoryA(path, NULL));
    MD_CHECK(!md_win_read_path(path, &bytes, &size, 4079));
    MD_CHECK(RemoveDirectoryA(path));
    /* Junctions require no symlink privilege and are real reparse points. */
    md_win_directory_close(d);
    char command[65536];
    snprintf(command, sizeof(command), "cmd /c mklink /J \"%s\" \"%s\" >nul", link, dir);
    MD_CHECK(system(command) == 0);
    MD_CHECK(!md_win_directory(link, false));
    char through[32768]; snprintf(through, sizeof(through), "%s/child", link);
    MD_CHECK(!md_win_directory(through, true));
    MD_CHECK(RemoveDirectoryA(link));
    MD_CHECK(md_test_win_acl(dir, false));
    MD_CHECK(!md_win_directory(dir, false));
    MD_CHECK(md_test_win_acl(dir, true));
    MD_CHECK(RemoveDirectoryA(dir)); free(dir);
}
/* Run this cell under a token with SeRestorePrivilege (SSH admin or an
 * elevated interactive task). No foreign-owned object survives the test. */
static void test_foreign_owner(void) {
    HANDLE token = NULL;
    TOKEN_PRIVILEGES enable = {0}, previous = {0}; DWORD previous_size = sizeof(previous);
    MD_CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token));
    if (!token) return;
    enable.PrivilegeCount = 1;
    MD_CHECK(LookupPrivilegeValueA(NULL, "SeRestorePrivilege", &enable.Privileges[0].Luid));
    enable.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    bool enabled = AdjustTokenPrivileges(token, FALSE, &enable, sizeof(previous), &previous, &previous_size) &&
        GetLastError() == ERROR_SUCCESS;
    MD_CHECK(enabled);
    if (!enabled) { CloseHandle(token); return; }
    char *dir = md_test_win_directory(); MD_CHECK(dir != NULL);
    MdWinDirectory *d = md_win_directory(dir, false);
    MD_CHECK(d && md_win_replace(d, "token", "owner-secret", 12));
    char path[32768]; snprintf(path, sizeof(path), "%s/token", dir);
    PSID system = NULL, owner = NULL; PSECURITY_DESCRIPTOR original = NULL;
    MD_CHECK(ConvertStringSidToSidA("S-1-5-18", &system));
    MD_CHECK(GetNamedSecurityInfoA(path, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        &owner, NULL, NULL, NULL, &original) == ERROR_SUCCESS);
    MD_CHECK(SetNamedSecurityInfoA(path, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        system, NULL, NULL, NULL) == ERROR_SUCCESS);
    unsigned char *bytes = NULL; size_t size = 0;
    MD_CHECK(!md_win_read_path(path, &bytes, &size, 4079) && !bytes);
    MD_CHECK(SetNamedSecurityInfoA(path, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        owner, NULL, NULL, NULL) == ERROR_SUCCESS);
    MD_CHECK(md_win_remove(d, "token"));
    md_win_directory_close(d); MD_CHECK(RemoveDirectoryA(dir)); free(dir);
    LocalFree(system); LocalFree(original);
    MD_CHECK(AdjustTokenPrivileges(token, FALSE, &previous, 0, NULL, NULL)); CloseHandle(token);
}
static void test_native_accounts(void) {
    char *dir = md_test_win_directory(); MD_CHECK(dir != NULL); if (!dir) return;
    MdAccounts *a = NULL;
    MD_CHECK(md_accounts_open("relative", NULL, &a) == MD_ACCOUNTS_IO && !a);
    MD_CHECK(md_test_win_acl(dir, false));
    MD_CHECK(md_accounts_open(dir, NULL, &a) == MD_ACCOUNTS_IO && !a);
    MD_CHECK(md_test_win_acl(dir, true));
    MD_CHECK(md_accounts_open(dir, NULL, &a) == 0);
    MD_CHECK(md_accounts_add(a, MD_SIGNER_DIRECT_KEY, pk, "Native account") == 0);
    MD_CHECK(md_accounts_save(a) == 0);
    md_accounts_destroy(a); a = NULL;
    char file[32768]; snprintf(file, sizeof(file), "%s/management.json", dir);
    MD_CHECK(md_test_win_acl(file, false));
    MD_CHECK(md_accounts_open(dir, NULL, &a) == MD_ACCOUNTS_IO && !a);
    MD_CHECK(md_test_win_acl(file, true));
    MdWinDirectory *d = md_win_directory(dir, false);
    MD_CHECK(md_win_replace(d, "management.json", "{\"version\":2}", 13));
    MD_CHECK(md_accounts_open(dir, NULL, &a) == MD_ACCOUNTS_INVALID && !a);
    MD_CHECK(md_win_remove(d, "management.json"));
    MD_CHECK(md_win_remove(d, ".management.lock"));
    md_win_directory_close(d);
    MD_CHECK(RemoveDirectoryA(dir)); free(dir);
}
static void test_dpapi(void) {
    char *dir = md_test_win_directory(); MD_CHECK(dir != NULL); if (!dir) return;
    /* Payload exceeds Credential Manager's 2560-byte maximum and roundtrips
     * through DPAPI in a fresh backend instance. */
    char relay[2049]; memset(relay, 'a', sizeof(relay)); memcpy(relay, "wss://", 6); relay[2048] = 0;
    char *relays[] = {relay, relay};
    MdNip46Credentials c = {.relay_count = 2, .relays = relays};
    strcpy(c.user_pubkey, pk); strcpy(c.remote_pubkey, other_pk); strcpy(c.client_secret, secret);
    MdCredentialStore *s = md_credential_store_windows(dir);
    MD_CHECK(s && md_credential_store_put(s, &c) == 0);
    md_credential_store_destroy(s);
    s = md_credential_store_windows(dir);
    MdNip46Credentials *loaded = NULL;
    MD_CHECK(md_credential_store_get(s, pk, &loaded) == 0);
    if (loaded) MD_CHECK(!strcmp(loaded->client_secret, secret) && strlen(loaded->relays[0]) == 2048);
    md_nip46_credentials_free(loaded); loaded = NULL;
    MdWinDirectory *d = md_win_directory(dir, false);
    char name[80]; snprintf(name, sizeof(name), "%s.dpapi", pk);
    HANDLE h = md_win_file(d, name, GENERIC_READ, OPEN_EXISTING, FILE_SHARE_READ);
    unsigned char *cipher = NULL; size_t size = 0;
    MD_CHECK(h != INVALID_HANDLE_VALUE && md_win_read(h, &cipher, &size, 131072));
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (cipher) {
        bool plaintext = false;
        for (size_t i = 0; i + strlen(secret) <= size; ++i)
            if (!memcmp(cipher + i, secret, strlen(secret))) plaintext = true;
        MD_CHECK(!plaintext);
        char other[80]; snprintf(other, sizeof(other), "%s.dpapi", other_pk);
        MD_CHECK(md_win_replace(d, other, cipher, size));
        MD_CHECK(md_credential_store_get(s, other_pk, &loaded) != 0 && !loaded);
        MD_CHECK(md_credential_store_remove(s, other_pk) == 0);
        cipher[size/2] ^= 0x80;
        MD_CHECK(md_win_replace(d, name, cipher, size));
        MD_CHECK(md_credential_store_get(s, pk, &loaded) != 0 && !loaded);
        md_credential_wipe(cipher, size); free(cipher);
    }
    MD_CHECK(md_credential_store_remove(s, pk) == 0);
    MD_CHECK(md_credential_store_remove(s, pk) == 0);
    MD_CHECK(md_credential_store_get(s, pk, &loaded) == MD_CREDENTIAL_NOT_FOUND && !loaded);
    md_win_directory_close(d); md_credential_store_destroy(s);
    MD_CHECK(RemoveDirectoryA(dir)); free(dir);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--foreign-owner")) {
        RUN_TEST(test_foreign_owner); return md_test_report();
    }
    RUN_TEST(test_native_files); RUN_TEST(test_native_accounts); RUN_TEST(test_dpapi);
    return md_test_report();
}
