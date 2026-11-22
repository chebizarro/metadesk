/* Explicit Cisco binary acquisition. No network calls from the encoder loader. */
#include "encode_openh264_fetch.h"
#include "encode_openh264_runtime.h"
#include "encode_openh264_license.h"
#include <bzlib.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <wchar.h>
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/acl.h>
#endif
#endif

/* Cisco's .signed.sha256.txt files hash the DECOMPRESSED, signed libraries.
 * Provenance, URLs and vetting procedure: encoder-runtime.md. Do not accept a
 * digest downloaded alongside the binary in place of these reviewed pins. */
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
#define ARTIFACT "openh264-2.7.0-win-arm64.dll"
#define DIGEST "2f8f11a40d1450fd5c7afc1c5e38b9f6ba6c95c47582845b16c0bfe154ed75a4"
#elif defined(_WIN32) && defined(_WIN64)
#define ARTIFACT "openh264-2.7.0-win64.dll"
#define DIGEST "0ef5e7f3b38753b15c13afa0418d4d7805fa228e6adfd3e425bbf758cff1c288"
#elif defined(_WIN32)
#define ARTIFACT "openh264-2.7.0-win32.dll"
#define DIGEST "ee34d9606c5cc96cdea694a6bc95ed1b6230b9854689f56c7fa30d211daf48b0"
#elif defined(__APPLE__) && defined(__aarch64__)
#define ARTIFACT "libopenh264-2.7.0-mac-arm64.dylib"
#define DIGEST "2e1ae3cd10c08328ca03a1f304fd2a1a2a7e5658c6d5a8582101a963f7995ef0"
#elif defined(__APPLE__) && defined(__x86_64__)
#define ARTIFACT "libopenh264-2.7.0-mac-x64.dylib"
#define DIGEST "31d38bcbe9348e9b44ca38dd08fa1e2af79ffce6521276449ef05e3cd39136f4"
#elif defined(__linux__) && defined(__aarch64__)
#define ARTIFACT "libopenh264-2.7.0-linux-arm64.8.so"
#define DIGEST "9ac664b0154fd0868491c2a904397b8a05779874c0603b8b1f16ad15de1ffd82"
#elif defined(__linux__) && defined(__x86_64__)
#define ARTIFACT "libopenh264-2.7.0-linux64.8.so"
#define DIGEST "1522f873a482923c5ec018a4415e1866898197528828c50f90fd300c94620df3"
#elif defined(__linux__) && defined(__i386__)
#define ARTIFACT "libopenh264-2.7.0-linux32.8.so"
#define DIGEST "7d095448e75ea49799e48cc11f079091f4c61b127dd71ece27b3496359102ecf"
#elif defined(__linux__) && defined(__arm__)
#define ARTIFACT "libopenh264-2.7.0-linux-arm.8.so"
#define DIGEST "c9702982cad7ae0da05ca1e62834666f56b0290484af463f0887c8c48043e82b"
#else
#define ARTIFACT ""
#define DIGEST ""
#endif
#define MAX_BINARY (16u * 1024u * 1024u)
#define MAX_DOWNLOAD (8u * 1024u * 1024u)

bool md_openh264_unpack(const void *input, size_t size, const char *digest,
                         unsigned char **output, size_t *output_size) {
    *output = NULL; *output_size = 0;
    if (!input || !size || size > MAX_DOWNLOAD || strlen(digest) != 64) return false;
    unsigned char *buf = malloc(MAX_BINARY);
    if (!buf) return false;
    bz_stream bz = {0};
    if (BZ2_bzDecompressInit(&bz, 0, 0) != BZ_OK) { free(buf); return false; }
    bz.next_in = (char *)input; bz.avail_in = (unsigned)size;
    bz.next_out = (char *)buf; bz.avail_out = MAX_BINARY;
    int result = BZ2_bzDecompress(&bz);
    size_t len = MAX_BINARY - bz.avail_out;
    bool complete = result == BZ_STREAM_END && bz.avail_in == 0 && len > 0;
    BZ2_bzDecompressEnd(&bz);
    unsigned char hash[EVP_MAX_MD_SIZE]; unsigned hash_len = 0;
    char hex[65];
    if (!complete || !EVP_Digest(buf, len, hash, &hash_len, EVP_sha256(), NULL) || hash_len != 32) {
        free(buf); return false;
    }
    for (unsigned i = 0; i < 32; i++) snprintf(hex + 2*i, 3, "%02x", hash[i]);
    if (strcmp(hex, digest)) { free(buf); return false; }
    *output = buf; *output_size = len;
    return true;
}

typedef struct { unsigned char *data; size_t size; } Download;
static size_t receive(void *data, size_t unit, size_t count, void *ctx) {
    Download *d = ctx;
    if (unit && count > SIZE_MAX / unit) return 0;
    size_t n = unit * count;
    if (n > MAX_DOWNLOAD - d->size) return 0;
    unsigned char *p = realloc(d->data, d->size + n);
    if (!p && n) return 0;
    d->data = p;
    if (n) memcpy(d->data + d->size, data, n);
    d->size += n;
    return n;
}

#ifndef _WIN32
/* macOS extended allow ACLs do not appear in st_mode. Reject them rather
 * than silently changing an existing directory or inheriting extra access. */
static bool private_acl(int fd) {
#ifdef __APPLE__
    acl_t acl = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
    if (!acl) return errno == ENOENT;
    if (acl_valid(acl)) { acl_free(acl); return false; }
    acl_entry_t entry;
    int result = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
    bool ok = true;
    while (result == 0) {
        acl_tag_t tag;
        if (acl_get_tag_type(entry, &tag) || tag == ACL_EXTENDED_ALLOW) { ok = false; break; }
        result = acl_get_entry(acl, ACL_NEXT_ENTRY, &entry);
    }
    /* Darwin returns -1 at end, errno EINVAL; ACL entry API is not POSIX's
     * 1/0 iteration convention. acl_get_entry docs and SDK are authoritative. */
    ok = ok && result == -1 && errno == EINVAL;
    acl_free(acl);
    return ok;
#else
    (void)fd;
    return true;
#endif
}
/* Walk with directory fds: no symlink traversal, including cache ancestors.
 * Only the metadesk/openh264 directories must be private; existing HOME/cache
 * ancestors may be shared but must not be writable by another user (except
 * root-owned sticky /tmp). The destination stays anchored through rename. */
static int cache_dir(void) {
    char path[4096];
    const char *home = getenv("HOME");
#ifndef __APPLE__
    const char *base = getenv("XDG_CACHE_HOME");
#endif
#ifdef __APPLE__
    int n = home && home[0] == '/' ? snprintf(path, sizeof(path), "%s/Library/Caches", home) : -1;
#else
    int n = base && base[0] == '/' ? snprintf(path, sizeof(path), "%s", base) :
            home && home[0] == '/' ? snprintf(path, sizeof(path), "%s/.cache", home) : -1;
#endif
    if (n <= 0 || (size_t)n >= sizeof(path)) return -1;
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char *save = NULL;
    for (char *part = strtok_r(path, "/", &save); fd >= 0 && part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) { close(fd); return -1; }
        if (mkdirat(fd, part, 0700) && errno != EEXIST) { close(fd); return -1; }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd); fd = next;
        struct stat st;
        if (fd >= 0 && (!private_acl(fd) || fstat(fd, &st) ||
            (st.st_uid != getuid() && st.st_uid != 0) ||
            ((st.st_mode & 0022) && !(st.st_uid == 0 && (st.st_mode & S_ISVTX))))) {
            close(fd); return -1;
        }
    }
    const char *dirs[] = {"metadesk", "openh264"};
    for (size_t i = 0; fd >= 0 && i < 2; i++) {
        if (mkdirat(fd, dirs[i], 0700) && errno != EEXIST) { close(fd); return -1; }
        int next = openat(fd, dirs[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd); fd = next;
        struct stat st;
        if (fd >= 0 && (!private_acl(fd) || fstat(fd, &st) || st.st_uid != getuid() || (st.st_mode & 0077))) {
            close(fd); return -1;
        }
    }
    return fd;
}
static bool install_binary(const void *buf, size_t size) {
    int dir = cache_dir();
    if (dir < 0) return false;
    unsigned char random[16]; char name[40] = ".fetch-";
    if (RAND_bytes(random, sizeof(random)) != 1) { close(dir); return false; }
    for (unsigned i = 0; i < sizeof(random); i++) snprintf(name + 7 + 2*i, 3, "%02x", random[i]);
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { close(dir); return false; }
    if (!private_acl(fd)) { close(fd); unlinkat(dir, name, 0); close(dir); return false; }
    size_t done = 0; bool ok = true;
    while (done < size) {
        ssize_t n = write(fd, (const char *)buf + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        done += (size_t)n;
    }
    if (ok && fsync(fd)) ok = false;
    if (close(fd)) ok = false;
    if (ok && renameat(dir, name, dir, MD_OH_NAME)) ok = false;
    if (ok && fsync(dir)) ok = false;
    if (!ok) unlinkat(dir, name, 0);
    close(dir);
    return ok;
}
#else
/* Validate and hold EVERY ancestor through publication. No pathname security
 * decision is reused after reopening; handles deny delete sharing and ACLs exclude untrusted mutation. */
static bool trusted_sid(PSID sid, PSID user) {
    if (EqualSid(sid, user) || IsWellKnownSid(sid, WinLocalSystemSid) ||
        IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) return true;
    BYTE installer[SECURITY_MAX_SID_SIZE]; DWORD bytes = sizeof(installer);
    wchar_t domain[256]; DWORD chars = 256; SID_NAME_USE type;
    return LookupAccountNameW(NULL, L"NT SERVICE\\TrustedInstaller", installer,
                             &bytes, domain, &chars, &type) && EqualSid(sid, installer);
}
static HANDLE open_directory(const wchar_t *path, PSID user, bool private) {
    HANDLE dir = CreateFileW(path, READ_CONTROL | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (dir == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "OpenH264: cannot hold cache ancestor (Windows error %lu)\n", GetLastError());
        return dir;
    }
    FILE_ATTRIBUTE_TAG_INFO attrs;
    PSID owner = NULL; PACL acl = NULL; PSECURITY_DESCRIPTOR sd = NULL;
    bool ok = GetFileInformationByHandleEx(dir, FileAttributeTagInfo, &attrs, sizeof(attrs)) &&
        (attrs.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(attrs.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    if (ok) ok = GetSecurityInfo(dir, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                &owner, NULL, &acl, NULL, &sd) == ERROR_SUCCESS;
    if (ok) ok = owner && acl && (private ? EqualSid(owner, user) : trusted_sid(owner, user));
    for (DWORD i = 0; ok && i < acl->AceCount; i++) {
        void *raw = NULL;
        if (!GetAce(acl, i, &raw)) { ok = false; break; }
        ACE_HEADER *h = raw;
        if (h->AceFlags & INHERIT_ONLY_ACE) continue;
        if (h->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            ACCESS_ALLOWED_ACE *ace = raw;
            /* ADD_SUBDIRECTORY alone on C:\ does not permit modifying existing
             * descendants. WRITE_DATA can change a directory's reparse data. */
            DWORD mutation = DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD |
                             FILE_WRITE_DATA | GENERIC_ALL | GENERIC_WRITE;
            if (private ? !EqualSid(&ace->SidStart, user) :
                ((ace->Mask & mutation) && !trusted_sid(&ace->SidStart, user))) ok = false;
        } else if (h->AceType != ACCESS_DENIED_ACE_TYPE) ok = false;
    }
    LocalFree(sd);
    if (!ok) {
        fputs("OpenH264: unsafe cache ancestor ownership, ACL or reparse point\n", stderr);
        CloseHandle(dir); SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
    }
    return dir;
}
static bool install_binary(const void *buf, size_t size) {
    wchar_t path[4096], temp[4096], target[4096];
    wchar_t base[4096];
    DWORD base_size = GetEnvironmentVariableW(L"LOCALAPPDATA", base, 4096);
    char utf8[4096];
    /* Match the loader's UTF-8 path bound, but obtain the OS environment wide. */
    if (!base_size || base_size >= 3800 || wcslen(base) < 3 || base[1] != L':' ||
        (base[2] != L'/' && base[2] != L'\\') ||
        !WideCharToMultiByte(CP_UTF8, 0, base, -1, utf8, 3800, NULL, NULL)) {
        fputs("OpenH264: invalid LOCALAPPDATA cache path\n", stderr); return false;
    }
    wcscpy(path, base);
    HANDLE token = NULL, file = INVALID_HANDLE_VALUE, dirs[128]; size_t count = 0;
    TOKEN_USER *tu = NULL; wchar_t *sid = NULL; PSECURITY_DESCRIPTOR sd = NULL;
    DWORD needed = 0; bool ok = false;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        fputs("OpenH264: cannot query current-user token\n", stderr); goto end;
    }
    GetTokenInformation(token, TokenUser, NULL, 0, &needed);
    tu = malloc(needed);
    if (!tu || !GetTokenInformation(token, TokenUser, tu, needed, &needed) ||
        !ConvertSidToStringSidW(tu->User.Sid, &sid)) {
        fputs("OpenH264: cannot obtain current-user SID\n", stderr); goto end;
    }
    /* Lock and validate the drive root and each component. No delete sharing
     * prevents ancestor renames during subsequent pathname operations. ACLs
     * exclude untrusted WRITE_DATA (reparse mutation), WRITE_DAC and DELETE_CHILD.
     * Write sharing is necessary for our child creation/rename operations. */
    for (wchar_t *p = path + 3; ; p++) {
        if (p == path + 3 || !*p || *p == L'/' || *p == L'\\') {
            wchar_t saved = *p; *p = 0;
            if (count == 128) { *p = saved; goto end; }
            HANDLE dir = open_directory(path, tu->User.Sid, false); *p = saved;
            if (dir == INVALID_HANDLE_VALUE) goto end;
            dirs[count++] = dir;
            if (!saved) break;
        }
    }
    wchar_t sddl[256];
    if (swprintf(sddl, 256, L"D:P(A;OICI;FA;;;%ls)", sid) < 0 ||
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL)) goto end;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
    const wchar_t *parts[] = {L"\\metadesk", L"\\openh264"};
    for (unsigned i = 0; i < 2; i++) {
        wcscat(path, parts[i]);
        if (!CreateDirectoryW(path, &sa) && GetLastError() != ERROR_ALREADY_EXISTS) {
            fprintf(stderr, "OpenH264: cannot create private cache (Windows error %lu)\n", GetLastError());
            goto end;
        }
        if (count == 128) goto end;
        HANDLE dir = open_directory(path, tu->User.Sid, true);
        if (dir == INVALID_HANDLE_VALUE) goto end;
        dirs[count++] = dir;
    }
    unsigned char random[16]; wchar_t suffix[33];
    if (RAND_bytes(random, sizeof(random)) != 1) goto end;
    for (unsigned i = 0; i < sizeof(random); i++) swprintf(suffix + 2*i, 3, L"%02x", random[i]);
    swprintf(temp, 4096, L"%ls\\.fetch-%ls", path, suffix);
    wchar_t filename[128];
    if (!MultiByteToWideChar(CP_UTF8, 0, MD_OH_NAME, -1, filename, 128)) goto end;
    swprintf(target, 4096, L"%ls\\%ls", path, filename);
    file = CreateFileW(temp, GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto end;
    DWORD written = 0;
    ok = WriteFile(file, buf, (DWORD)size, &written, NULL) && written == size && FlushFileBuffers(file);
    if (!CloseHandle(file)) ok = false;
    file = INVALID_HANDLE_VALUE;
    if (ok) ok = MoveFileExW(temp, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temp);
end:
    if (!ok) fprintf(stderr, "OpenH264: safe cache installation failed (Windows error %lu)\n", GetLastError());
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    while (count) CloseHandle(dirs[--count]);
    LocalFree(sd); LocalFree(sid); free(tu);
    if (token) CloseHandle(token);
    return ok;
}

#endif

int md_openh264_fetch(void) {
    /* Flush notice before initiating any network activity, including DNS. */
    fputs(md_openh264_binary_license, stdout);
    fputs("\nExplicit download: https://ciscobinary.openh264.org/" ARTIFACT ".bz2\n"
          "Verifying pinned Cisco SHA-256: " DIGEST "\n", stdout);
    if (fflush(stdout)) return 1;
    if (!ARTIFACT[0]) { fputs("Unsupported OpenH264 platform/architecture\n", stderr); return 1; }
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) return 1;
    CURL *curl = curl_easy_init();
    Download d = {0};
    bool ok = curl != NULL;
#define SET(option, value) do { if (ok && curl_easy_setopt(curl, option, value) != CURLE_OK) ok = false; } while (0)
    SET(CURLOPT_URL, "https://ciscobinary.openh264.org/" ARTIFACT ".bz2");
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, "https");
#else
    SET(CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_FAILONERROR, 1L);
    SET(CURLOPT_CONNECTTIMEOUT, 30L);
    SET(CURLOPT_TIMEOUT, 180L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, receive);
    SET(CURLOPT_WRITEDATA, &d);
#undef SET
    if (ok) ok = curl_easy_perform(curl) == CURLE_OK;
    long status = 0;
    if (ok) ok = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status == 200;
    curl_easy_cleanup(curl); curl_global_cleanup();
    unsigned char *binary = NULL; size_t size = 0;
    if (ok) ok = md_openh264_unpack(d.data, d.size, DIGEST, &binary, &size);
    free(d.data);
    if (ok) ok = install_binary(binary, size);
    free(binary);
    fputs(ok ? "Verified Cisco OpenH264 2.7.0 installed in per-user cache.\n" :
               "OpenH264 fetch failed: download, digest, or safe cache installation rejected.\n", stderr);
    return ok ? 0 : 1;
}
