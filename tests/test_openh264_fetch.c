#include "encode_openh264_fetch.c"
#include <assert.h>
#ifdef __APPLE__
#include <membership.h>
#endif
/* Fixture assertions stay enabled even under release builds. */
#undef assert
#define assert(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
int main(void) {
    const char plain[] = "offline fixture, not an installable codec";
    char compressed[512]; unsigned n = sizeof(compressed);
    assert(BZ2_bzBuffToBuffCompress(compressed, &n, (char *)plain, sizeof(plain), 9, 0, 30) == BZ_OK);
    unsigned char hash[32]; unsigned len; char digest[65];
    assert(EVP_Digest(plain, sizeof(plain), hash, &len, EVP_sha256(), NULL));
    for (unsigned i = 0; i < 32; i++) snprintf(digest + 2*i, 3, "%02x", hash[i]);
    unsigned char *out = NULL; size_t size = 0;
    assert(md_openh264_unpack(compressed, n, digest, &out, &size));
    assert(size == sizeof(plain) && !memcmp(out, plain, size)); free(out);
    assert(!md_openh264_unpack(compressed, n - 1, digest, &out, &size));
    assert(!out && !size);
    compressed[n] = 'x';
    assert(!md_openh264_unpack(compressed, n + 1, digest, &out, &size));
    assert(!md_openh264_unpack(compressed, n, DIGEST, &out, &size)); /* wrong arch/version/content */
    compressed[0] ^= 1; /* corrupt the stream magic, not an unused padding bit */
    assert(!md_openh264_unpack(compressed, n, digest, &out, &size));
    assert(!md_openh264_unpack(compressed, MAX_DOWNLOAD + 1, digest, &out, &size));
    assert(!md_openh264_unpack(NULL, 0, digest, &out, &size));
    unsigned char *large = malloc(MAX_BINARY + 1); assert(large);
    memset(large, 'x', MAX_BINARY + 1);
    char bomb[4096]; unsigned bomb_size = sizeof(bomb);
    assert(BZ2_bzBuffToBuffCompress(bomb, &bomb_size, (char *)large, MAX_BINARY + 1, 9, 0, 30) == BZ_OK);
    free(large);
    assert(!md_openh264_unpack(bomb, bomb_size, digest, &out, &size));
    Download capped = {.size = MAX_DOWNLOAD};
    assert(receive((void *)plain, 1, 1, &capped) == 0);
    assert(receive((void *)plain, SIZE_MAX, 2, &capped) == 0);
    assert(md_openh264_version_supported(2, 6, 0, 2502));
    assert(md_openh264_version_supported(2, 7, 0, 2610));
    assert(!md_openh264_version_supported(2, 7, 0, 2611));
    assert(!md_openh264_version_supported(2, 6, 1, 2502));
    assert(!md_openh264_version_supported(2, 5, 0, 2502));
#ifndef _WIN32
    char home[] = "/private/tmp/md-w3-offline-XXXXXX";
#ifndef __APPLE__
    strcpy(home, "/tmp/md-w3-offline-XXXXXX");
#endif
    assert(mkdtemp(home));
    assert(setenv("HOME", home, 1) == 0);
    assert(unsetenv("XDG_CACHE_HOME") == 0);
    assert(install_binary(plain, sizeof(plain)));
    int fd = cache_dir(); assert(fd >= 0);
    struct stat st; assert(fstatat(fd, MD_OH_NAME, &st, AT_SYMLINK_NOFOLLOW) == 0);
    assert((st.st_mode & 0777) == 0600);
    assert(unlinkat(fd, MD_OH_NAME, 0) == 0);
    /* Atomic replacement does not follow a preexisting target symlink. */
    assert(symlinkat("/must-not-write-here", fd, MD_OH_NAME) == 0);
    assert(install_binary(plain, sizeof(plain)));
    assert(fstatat(fd, MD_OH_NAME, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode));
    assert(unlinkat(fd, MD_OH_NAME, 0) == 0);
    assert(fchmod(fd, 0777) == 0); assert(!install_binary(plain, sizeof(plain)));
    assert(fchmod(fd, 0700) == 0);
#ifdef __APPLE__
    /* An access-granting ACL is invisible in the Unix 0700 mode bits. */
    acl_t acl = acl_init(1); acl_entry_t entry; acl_permset_t perms;
    uuid_t other; assert(mbr_uid_to_uuid(65534, other) == 0);
    assert(acl_create_entry(&acl, &entry) == 0);
    assert(acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0);
    assert(acl_set_qualifier(entry, other) == 0);
    assert(acl_get_permset(entry, &perms) == 0);
    assert(acl_add_perm(perms, ACL_ADD_FILE) == 0);
    assert(acl_add_perm(perms, ACL_DELETE_CHILD) == 0);
    assert(acl_set_fd_np(fd, acl, ACL_TYPE_EXTENDED) == 0);
    assert(fstat(fd, &st) == 0 && (st.st_mode & 0777) == 0700);
    assert(!install_binary(plain, sizeof(plain)));
    acl_flagset_t flags;
    assert(acl_get_flagset_np(entry, &flags) == 0);
    assert(acl_add_flag_np(flags, ACL_ENTRY_DIRECTORY_INHERIT) == 0);
    assert(acl_add_flag_np(flags, ACL_ENTRY_FILE_INHERIT) == 0);
    assert(acl_set_fd_np(fd, acl, ACL_TYPE_EXTENDED) == 0);
    assert(!install_binary(plain, sizeof(plain))); /* reject inherited grants too */
    acl_free(acl); acl = acl_init(0);
    assert(acl_set_fd_np(fd, acl, ACL_TYPE_EXTENDED) == 0); acl_free(acl);
#endif
    close(fd);

    char cache[256], vendor[512], leaf[768];
#ifdef __APPLE__
    snprintf(cache, sizeof(cache), "%s/Library/Caches", home);
#else
    snprintf(cache, sizeof(cache), "%s/.cache", home);
#endif
    snprintf(vendor, sizeof(vendor), "%s/metadesk", cache);
    snprintf(leaf, sizeof(leaf), "%s/openh264", vendor);
    assert(rmdir(leaf) == 0); assert(symlink("/", leaf) == 0);
    assert(!install_binary(plain, sizeof(plain)));
    assert(unlink(leaf) == 0); assert(rmdir(vendor) == 0); assert(rmdir(cache) == 0);
#ifdef __APPLE__
    snprintf(cache, sizeof(cache), "%s/Library", home); assert(rmdir(cache) == 0);
#endif
    assert(rmdir(home) == 0);
#else
    wchar_t root[32768], temp[32768]; char utf8[32768];
    assert(GetTempPathW(32000, root));
    assert(GetTempFileNameW(root, L"mdo", 0, temp));
    assert(DeleteFileW(temp)); assert(CreateDirectoryW(temp, NULL));
    assert(WideCharToMultiByte(CP_UTF8, 0, temp, -1, utf8, sizeof(utf8), NULL, NULL));
    /* Force a Unicode cache path even with a non-UTF-8 CRT environment. */
    wchar_t unicode[32768]; swprintf(unicode, 32768, L"%ls\\cache-\u96ea", temp);
    assert(CreateDirectoryW(unicode, NULL));
    assert(SetEnvironmentVariableW(L"LOCALAPPDATA", unicode));
    assert(install_binary(plain, sizeof(plain)));
    assert(install_binary(plain, sizeof(plain))); /* replacement */
    wchar_t file[32768], leaf[32768], vendor[32768], filename[128];
    assert(MultiByteToWideChar(CP_UTF8, 0, MD_OH_NAME, -1, filename, 128));
    swprintf(vendor, 32768, L"%ls\\metadesk", unicode);
    swprintf(leaf, 32768, L"%ls\\openh264", vendor);
    swprintf(file, 32768, L"%ls\\%ls", leaf, filename);
    /* Held validated directories cannot be replaced between check and use. */
    HANDLE token = NULL; DWORD bytes = 0;
    assert(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
    TOKEN_USER *user = malloc(bytes); assert(user);
    assert(GetTokenInformation(token, TokenUser, user, bytes, &bytes));
    HANDLE held = open_directory(leaf, user->User.Sid, true);
    assert(held != INVALID_HANDLE_VALUE);
    wchar_t moved[32768]; swprintf(moved, 32768, L"%ls-moved", leaf);
    assert(!MoveFileExW(leaf, moved, 0));
    CloseHandle(held); CloseHandle(token); free(user);
    assert(DeleteFileW(file)); assert(RemoveDirectoryW(leaf));
    /* Existing inherited, non-private ACL must be rejected. */
    assert(CreateDirectoryW(leaf, NULL));
    PSECURITY_DESCRIPTOR bad = NULL;
    assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;WD)", SDDL_REVISION_1, &bad, NULL));
    assert(SetFileSecurityW(leaf, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, bad));
    LocalFree(bad);
    assert(!install_binary(plain, sizeof(plain)));
    assert(RemoveDirectoryW(leaf)); assert(RemoveDirectoryW(vendor));
    /* An otherwise private cache below an attacker-writable ancestor fails. */
    PSECURITY_DESCRIPTOR mutable = NULL;
    assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;WD)", SDDL_REVISION_1, &mutable, NULL));
    assert(SetFileSecurityW(unicode, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, mutable));
    LocalFree(mutable);
    assert(!install_binary(plain, sizeof(plain)));
    assert(RemoveDirectoryW(unicode)); assert(RemoveDirectoryW(temp));
#endif
    puts("PASS offline: bounded unpack, tamper/truncation/trailing bytes, pins, version allowlist, private atomic cache");
    return 0;
}
