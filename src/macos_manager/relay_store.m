#import "native_relay_store.h"
#include "accounts.h"
#include "identity_resolver.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

NSString *MDRelayConfigDirectory(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) return [@(xdg) stringByAppendingPathComponent:@"metadesk"];
    return [(home && *home ? @(home) : NSHomeDirectory()) stringByAppendingPathComponent:@".config/metadesk"];
}

static int Hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if ((c | 32) >= 'a' && (c | 32) <= 'f') return (c | 32) - 'a' + 10;
    return -1;
}

static BOOL Unreserved(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || (c && strchr("-._~", c));
}

static void Escape(NSMutableString *out, unsigned char c) { [out appendFormat:@"%%%02X", (unsigned)c]; }

// GUri's encoded-path mode decodes only unreserved octets; other escapes
// survive in uppercase. Literal non-ASCII bytes are escaped, not decoded.
static NSString *Path(const char *path) {
    NSMutableString *encoded = [NSMutableString string];
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        unsigned char c = *p;
        if (c == '%') {
            if (!p[1] || !p[2] || Hex(p[1]) < 0 || Hex(p[2]) < 0) return nil;
            c = (unsigned char)(16 * Hex(p[1]) + Hex(p[2])); p += 2;
            if (!Unreserved(c)) { Escape(encoded, c); continue; }
        } else if (c >= 128) { Escape(encoded, c); continue; }
        [encoded appendFormat:@"%c", c];
    }
    if (!encoded.length) return @"/";
    // Remove dot segments without collapsing consecutive slashes.
    NSArray *pieces = [[encoded substringFromIndex:1] componentsSeparatedByString:@"/"];
    NSMutableArray *kept = [NSMutableArray array];
    for (NSUInteger i = 0; i < pieces.count; ++i) {
        NSString *piece = pieces[i];
        BOOL parent = [piece isEqualToString:@".."];
        if (parent || [piece isEqualToString:@"."]) {
            if (parent) [kept removeLastObject];
            if (i + 1 == pieces.count) [kept addObject:@""];
        } else [kept addObject:piece];
    }
    return [@"/" stringByAppendingString:[kept componentsJoinedByString:@"/"]];
}

static NSString *Host(char *host, BOOL ipv6) {
    NSString *text = @(host);
    if (!text.length) return nil;
    NSString *decoded = text.stringByRemovingPercentEncoding;
    if (!decoded.length || strlen(decoded.UTF8String) != [decoded lengthOfBytesUsingEncoding:NSUTF8StringEncoding]) return nil;
    if (!ipv6) {
        struct in_addr address;
        if (strchr(host, '%') && inet_pton(AF_INET, decoded.UTF8String, &address) == 1) return nil;
        if (![decoded canBeConvertedToEncoding:NSASCIIStringEncoding]) {
            // Apply Foundation's IDNA conversion label by label so an empty
            // interior label accepted by the GTK store is preserved.
            NSArray *labels = [decoded componentsSeparatedByCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@".。．｡"]];
            NSMutableArray *ascii = [NSMutableArray array];
            for (NSString *label in labels) {
                if ([label canBeConvertedToEncoding:NSASCIIStringEncoding]) { [ascii addObject:label]; continue; }
                NSURLComponents *parts = [NSURLComponents new];
                parts.scheme = @"ws";
                parts.host = label;
                NSString *name = parts.URL.host;
                if (!name.length) {
                    // IDNA can map a whole label to nothing (e.g. U+200B).
                    // Distinguish that from an invalid label using a sentinel.
                    parts.host = [label stringByAppendingString:@"x"];
                    if (![parts.URL.host isEqualToString:@"x"]) return nil;
                    name = @"";
                }
                if (![name canBeConvertedToEncoding:NSASCIIStringEncoding]) return nil;
                [ascii addObject:name];
            }
            decoded = [ascii componentsJoinedByString:@"."];
            if (!decoded.length) return nil;
        }
    }
    NSMutableString *escaped = [NSMutableString string];
    for (const unsigned char *p = (const unsigned char *)decoded.UTF8String; *p; ++p) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (Unreserved(c) || (ipv6 ? c == ':' : strchr("!$&'()*+,;=", c) != NULL)) [escaped appendFormat:@"%c", c];
        else Escape(escaped, c);
    }
    return ipv6 ? [NSString stringWithFormat:@"[%@]", escaped] : escaped;
}

NSString *MDRelayNormalize(const char *url) {
    if (!url || !*url || strnlen(url, MD_RELAY_URL_MAX + 1) > MD_RELAY_URL_MAX) return nil;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        if (*p < 0x21 || *p == 0x7f || strchr("\\?#", *p)) return nil;
    BOOL secure = !strncasecmp(url, "wss://", 6);
    if (!secure && strncasecmp(url, "ws://", 5)) return nil;
    const char *authority = url + (secure ? 6 : 5);
    const char *path = authority + strcspn(authority, "/");
    char authorityCopy[MD_RELAY_URL_MAX + 1];
    memcpy(authorityCopy, authority, (size_t)(path - authority));
    authorityCopy[path - authority] = 0;
    if (strchr(authorityCopy, '@')) return nil;
    char *host = authorityCopy, *port = NULL;
    BOOL ipv6 = *host == '[';
    if (ipv6) {
        char *end = strchr(++host, ']');
        if (!end || (end[1] && end[1] != ':')) return nil;
        if (end[1]) port = end + 2;
        *end = 0;
        char *zone = strchr(host, '%');
        if (zone) {
            if (strncmp(zone, "%25", 3) || !zone[3]) return nil;
            *zone = 0;
        }
        struct in6_addr address;
        if (inet_pton(AF_INET6, host, &address) != 1) return nil;
        if (zone) *zone = '%';
    } else {
        port = strchr(host, ':');
        if (port) *port++ = 0;
    }
    unsigned number = 0;
    if (port && *port) {
        for (const char *p = port; *p; ++p) {
            if (*p < '0' || *p > '9' || (number = 10 * number + (*p - '0')) > 65535) return nil;
        }
        if (!number) return nil;
        if (number == (secure ? 443u : 80u)) number = 0;
    }
    NSString *name = Host(host, ipv6), *resource = Path(path);
    if (!name || !resource) return nil;
    return [NSString stringWithFormat:@"%@://%@%@%@", secure ? @"wss" : @"ws", name,
            number ? [NSString stringWithFormat:@":%u", number] : @"", resource];
}

static BOOL Space(unsigned char c) { return c && strchr(" \t\r\n\f", c); }

static void TrimRight(char *s) {
    char *end = s + strlen(s);
    while (end > s && Space(end[-1])) *--end = 0;
}

static BOOL Key(const char *key) {
    if (!*key) return NO;
    const char *locale = strpbrk(key, "[]");
    if (!locale) return YES;
    if (locale == key || *locale != '[' || locale[-1] == ' ') return NO;
    const char *end = strchr(++locale, ']');
    if (!end || end[1]) return NO;
    NSString *tag = [[NSString alloc] initWithBytes:locale length:(size_t)(end - locale) encoding:NSUTF8StringEncoding];
    if (!tag) return NO;
    NSMutableCharacterSet *allowed = [NSCharacterSet.alphanumericCharacterSet mutableCopy];
    [allowed formIntersectionWithCharacterSet:NSCharacterSet.nonBaseCharacterSet.invertedSet];
    [allowed addCharactersInString:@"-_.@"];
    return [tag rangeOfCharacterFromSet:allowed.invertedSet].location == NSNotFound;
}

static NSArray<NSString *> *RelayList(NSMutableData *value) {
    if (!value || ![[NSString alloc] initWithBytes:value.bytes length:value.length - 1 encoding:NSUTF8StringEncoding]) return nil;
    NSMutableArray *relays = [NSMutableArray array];
    char *p = value.mutableBytes;
    while (*p) {
        char *start = p, *out = p;
        while (*p && *p != ';') {
            char c = *p++;
            if (c == '\\') switch (c = *p++) {
                case 's': c = ' '; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case ';': case '\\': break;
                default: return nil;
            }
            *out++ = c;
        }
        if (*p) ++p;
        *out = 0;
        NSString *relay = MDRelayNormalize(start);
        if (!relay) return nil;
        [relays addObject:relay];
    }
    return relays;
}

NSArray<NSString *> *MDRelayLoad(NSString *directory) {
    NSString *path = [directory stringByAppendingPathComponent:@"manager-relays.ini"];
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? @[] : nil;
    struct stat st;
    FILE *file = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ? fdopen(fd, "r") : NULL;
    if (!file) { close(fd); return nil; }
    char *line = NULL, *startGroup = NULL;
    size_t cap = 0;
    ssize_t len;
    BOOL relays = NO, first = NO, valid = YES;
    NSMutableData *value = nil;
    while ((len = getline(&line, &cap, file)) >= 0) {
        if (len && line[len - 1] == '\n') {
            --len;
            if (len && line[len - 1] == '\r') --len;
            line[len] = 0;
        }
        char *p = line;
        while (Space(*p)) ++p;
        if (!*p || *p == '#') continue;
        if (*p == '[') {
            char *end = strchr(++p, ']');
            if (!end || end == p || end[1 + strspn(end + 1, " \t")]) { valid = NO; break; }
            *end = 0;
            for (const unsigned char *name = (const unsigned char *)p; *name; ++name)
                if (*name < 32 || *name == 127 || *name == '[') valid = NO;
            if (!valid) break;
            if (!startGroup && !(startGroup = strdup(p))) { valid = NO; break; }
            first = !strcmp(p, startGroup);
            relays = !strcmp(p, "relays");
        } else {
            char *equals = strchr(p, '=');
            if (!startGroup || !equals) { valid = NO; break; }
            *equals++ = 0;
            TrimRight(p);
            if (!Key(p)) { valid = NO; break; }
            while (Space(*equals)) ++equals;
            if (first && !strcmp(p, "Encoding") && strcasecmp(equals, "UTF-8")) { valid = NO; break; }
            if (relays && !strcmp(p, "bootstrap")) value = [NSMutableData dataWithBytes:equals length:strlen(equals) + 1];
        }
    }
    valid = valid && feof(file) && !ferror(file);
    free(line);
    free(startGroup);
    fclose(file);
    return valid ? RelayList(value) : nil;
}

BOOL MDRelaySave(NSString *directory, NSArray<NSString *> *relays) {
    NSMutableOrderedSet<NSString *> *normalized = [NSMutableOrderedSet orderedSet];
    for (NSString *relay in relays) {
        const char *p = relay.UTF8String;
        if (!p || strlen(p) != [relay lengthOfBytesUsingEncoding:NSUTF8StringEncoding]) return NO;
        NSString *url = MDRelayNormalize(p);
        if (!url) return NO;
        [normalized addObject:url];
    }
    NSMutableString *ini = [NSMutableString stringWithString:@"[relays]\nbootstrap="];
    for (NSString *url in normalized) [ini appendFormat:@"%@;", [url stringByReplacingOccurrencesOfString:@";" withString:@"\\;"]];
    [ini appendString:@"\n"];

    // Use the same guard as the GTK writer, including its private-directory
    // checks and non-blocking writer lock. No credential backend is opened.
    MdAccounts *guard = NULL;
    if (md_accounts_open(directory.fileSystemRepresentation, NULL, &guard)) return NO;
    NSString *path = [directory stringByAppendingPathComponent:@"manager-relays.ini"];
    char *temp = strdup([[path stringByAppendingString:@".XXXXXX"] fileSystemRepresentation]);
    int fd = temp ? mkstemp(temp) : -1;
    BOOL ok = NO;
    if (fd >= 0) {
        const char *bytes = ini.UTF8String;
        size_t remaining = strlen(bytes);
        while (remaining) {
            ssize_t n = write(fd, bytes, remaining);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            bytes += n; remaining -= (size_t)n;
        }
        ok = !remaining && fsync(fd) == 0;
        if (close(fd) != 0) ok = NO;
        ok = ok && rename(temp, path.fileSystemRepresentation) == 0;
        if (!ok) unlink(temp);
        else {
            int parent = open(directory.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            ok = parent >= 0 && fsync(parent) == 0;
            if (parent >= 0) close(parent);
        }
    }
    free(temp);
    md_accounts_destroy(guard);
    return ok;
}
