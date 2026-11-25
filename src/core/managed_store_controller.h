#ifndef MD_MANAGED_STORE_CONTROLLER_H
#define MD_MANAGED_STORE_CONTROLLER_H

#include "managed_store.h"
#include "relay_plan.h"

typedef struct MdStoreController MdStoreController;
typedef enum { MD_SYNC_OFFLINE, MD_SYNC_CATCHUP, MD_SYNC_READY, MD_SYNC_REFUSED,
               MD_SYNC_REPLAY_NEEDED, MD_SYNC_AUTH_PENDING, MD_SYNC_AUTH_REFUSED } MdStoreSync;
typedef enum { MD_EDIT_STAGE, MD_EDIT_KEEP_REMOTE, MD_EDIT_REAPPLY,
               MD_EDIT_REPLACE_INVALID } MdStoreEdit;

/* Call on one owner thread (typically a frontend worker). Changed is a wakeup
 * hint, including from transport threads; it must not call back or block. */
MdStoreController *md_store_controller_open(MdAccountBinding *, const char *state_directory,
    void (*changed)(void *), void *data);
void md_store_controller_close(MdStoreController *);
/* Explicit start/configuration change, not a timer. NULL/no relays means offline.
 * Changing route configuration retires every old subscription and EOSE barrier. */
bool md_store_controller_start(MdStoreController *, const MdRelayPlan *bootstrap);
/* Watch additional exact coordinates, e.g. tags learned from app-data. */
bool md_store_controller_watch(MdStoreController *, const char *coordinate);
/* Replace the affected persistent REQ to repair a failed local admission. */
bool md_store_controller_replay(MdStoreController *, const char *coordinate);
MdStoreSync md_store_controller_sync(MdStoreController *, const char *coordinate);
/* Wait for an already-issued scoped REQ; deadline is a failure watchdog, not
 * a completion criterion. Does not close or restart any subscription. */
MdStoreSync md_store_controller_wait(MdStoreController *, const char *coordinate, uint64_t deadline_ns);
MdStoreResult md_store_controller_read(MdStoreController *, const char *coordinate,
    char revision[65], char **winner, char **draft);
MdStoreResult md_store_controller_edit(MdStoreController *, const char *coordinate,
    MdStoreEdit, const char *revision, const char *text);
/* Send the already durable signed event to outstanding write targets. A retry
 * does not generate a new event. No transport means no outgoing hand-off. */
MdStoreResult md_store_controller_flush(MdStoreController *, const char *coordinate);
char *md_store_controller_recents(MdStoreController *);
MdStoreResult md_store_controller_record(MdStoreController *, const char *, MdRecentOutcome);
#endif
