#ifndef MD_DISCOVERY_HTTP_H
#define MD_DISCOVERY_HTTP_H

#include <stdbool.h>
#include <stdint.h>

/* Returned document is malloc-owned. An explicit operator URL may opt out of
 * the public-address policy. Redirects are never followed. */
char *md_discovery_http_get(const char *url, const char *accept, bool public_only,
                            long timeout_ms, bool *unsafe);
char *md_discovery_http_get_cancellable(const char *url, const char *accept, bool public_only,
    long timeout_ms, bool *unsafe, bool (*cancelled)(void *), void *data);
bool md_discovery_public_host(const char *host, uint64_t deadline_ns);
bool md_discovery_http_init(void);

#endif
