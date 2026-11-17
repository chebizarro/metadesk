#include "credential_store.h"
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
typedef struct { SecKeychainRef keychain; } Keychain;
/* macOS still supports private file Keychains for non-interactive services and
 * isolated tests. Never add these test Keychains to the user's search list. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static CFMutableDictionaryRef query(Keychain *k, const char *pk, int add) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef account = CFStringCreateWithCString(NULL, pk, kCFStringEncodingUTF8);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, CFSTR("org.metadesk.Nip46Credential"));
    CFDictionarySetValue(q, kSecAttrAccount, account); CFRelease(account);
    CFDictionarySetValue(q, kSecAttrSynchronizable, kCFBooleanFalse);
    CFDictionarySetValue(q, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
    if (k->keychain) {
        if (add) CFDictionarySetValue(q, kSecUseKeychain, k->keychain);
        else {
            CFArrayRef a = CFArrayCreate(NULL, (const void **)&k->keychain, 1, &kCFTypeArrayCallBacks);
            CFDictionarySetValue(q, kSecMatchSearchList, a); CFRelease(a);
        }
    }
    return q;
}
static int status_error(OSStatus s) {
    if (s == errSecSuccess) return 0;
    if (s == errSecItemNotFound) return MD_CREDENTIAL_NOT_FOUND;
    if (s == errSecInteractionNotAllowed || s == errSecAuthFailed || s == errSecUserCanceled)
        return MD_CREDENTIAL_LOCKED;
    if (s == errSecNotAvailable) return MD_CREDENTIAL_UNAVAILABLE;
    return MD_CREDENTIAL_FAILED;
}
static int kc_get(void *ctx, const char *pk, char **out) {
    CFMutableDictionaryRef q = query(ctx, pk, 0);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFTypeRef result = NULL;
    OSStatus status = SecItemCopyMatching(q, &result); CFRelease(q);
    int rc = status_error(status);
    if (!rc) {
        if (!result || CFGetTypeID(result) != CFDataGetTypeID()) rc = MD_CREDENTIAL_INVALID;
        else {
            CFIndex n = CFDataGetLength((CFDataRef)result);
            const UInt8 *b = CFDataGetBytePtr((CFDataRef)result);
            if (n < 0 || n > 65536 || memchr(b, 0, (size_t)n)) rc = MD_CREDENTIAL_INVALID;
            else {
                *out = malloc((size_t)n + 1);
                if (!*out) rc = MD_CREDENTIAL_FAILED;
                else { memcpy(*out, b, (size_t)n); (*out)[n] = 0; }
            }
        }
    }
    if (result) CFRelease(result); return rc;
}
static int kc_put(void *ctx, const char *pk, const char *payload) {
    CFMutableDictionaryRef q = query(ctx, pk, 0);
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)payload, (CFIndex)strlen(payload));
    CFMutableDictionaryRef values = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(values, kSecValueData, data);
    OSStatus status = SecItemUpdate(q, values); CFRelease(q);
    if (status == errSecItemNotFound) {
        q = query(ctx, pk, 1);
        CFDictionarySetValue(q, kSecValueData, data);
        CFDictionarySetValue(q, kSecAttrLabel, CFSTR("Metadesk remote signer"));
        status = SecItemAdd(q, NULL); CFRelease(q);
    }
    CFRelease(values); CFRelease(data); return status_error(status);
}
static int kc_remove(void *ctx, const char *pk) {
    CFMutableDictionaryRef q = query(ctx, pk, 0);
    int rc = status_error(SecItemDelete(q)); CFRelease(q);
    return rc == MD_CREDENTIAL_NOT_FOUND ? 0 : rc;
}
static void kc_destroy(void *ctx) {
    Keychain *k = ctx;
    if (k->keychain) CFRelease(k->keychain);
    free(k);
}
MdCredentialStore *md_credential_store_keychain(const char *path) {
    Keychain *k = calloc(1, sizeof(*k));
    if (!k) return NULL;
    if (path && SecKeychainOpen(path, &k->keychain) != errSecSuccess) { free(k); return NULL; }
    static const MdCredentialStoreOps ops = {kc_get, kc_put, kc_remove, kc_destroy};
    MdCredentialStore *s = md_credential_store_custom(&ops, k);
    if (!s) kc_destroy(k);
    return s;
}
#pragma clang diagnostic pop
#else
MdCredentialStore *md_credential_store_keychain(const char *path) { (void)path; return NULL; }
#endif
