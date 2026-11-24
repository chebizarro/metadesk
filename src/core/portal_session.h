#ifndef MD_PORTAL_SESSION_H
#define MD_PORTAL_SESSION_H

#include "capture.h"

typedef struct MdPortalSession MdPortalSession;

/* Capture retains one reference across stop/start. The input backend takes
 * another when attaching; the final release closes the session and EIS. */
MdPortalSession *md_portal_session_create(const MdCaptureConfig *capture,
                                          uint32_t timeout_ms, int *status);
void md_portal_session_unref(MdPortalSession *session);
int md_portal_session_open_capture(MdPortalSession *session, uint32_t *node);
bool md_portal_session_revoked(const MdPortalSession *session);
/* Callback runs under the session lock, and must not call session methods.
 * Unregister before freeing its data; unregister waits for it to finish. */
void md_portal_session_watch(MdPortalSession *session, void (*wake)(void *), void *data);
/* Borrowed while the calling thread owns the capture context. */
MdPortalSession *md_capture_portal_session(const MdCaptureCtx *capture);

#endif
