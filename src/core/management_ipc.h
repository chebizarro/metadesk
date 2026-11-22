#ifndef MD_MANAGEMENT_IPC_H
#define MD_MANAGEMENT_IPC_H
#include "host_access.h"
#include "nostr.h"
#include <cjson/cJSON.h>
typedef struct MdManagementIpc MdManagementIpc;
/* NULL path selects the identity-scoped runtime endpoint. Explicit paths must
 * have a canonical private owner-only parent. Existing endpoints are never
 * unlinked, including stale sockets. Windows returns unsupported. */
MdManagementIpc *md_management_start(MdHostAccess *access, MdNostr *nostr,
    const char *host_key, const char *path, uint32_t supported, void (*wake)(void *), void *data);
/* Stop nostr before joining: a pending remember may be waiting for relay OK. */
void md_management_stop(MdManagementIpc *s);
int md_management_path(const char *host_key, char *out, size_t size);
/* Uses real kernel credentials; expected_uid normally geteuid(). Public for
 * credential-rejection testing without changing a test process's privileges. */
bool md_management_peer_is_user(int fd, uint64_t expected_uid);
/* One request/reply with credential verification in both directions. */
cJSON *md_management_call(const char *path, const cJSON *request);
int md_host_management_cli(const char *area, int argc, char **argv);
#endif
