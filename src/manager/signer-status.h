#pragma once
#include <gio/gio.h>

#define MD_TYPE_SIGNER_STATUS (md_signer_status_get_type())
G_DECLARE_FINAL_TYPE(MdSignerStatus, md_signer_status, MD, SIGNER_STATUS, GObject)

/* Read-only Control1 adapter. No custody, pause, or permission mutation. */
MdSignerStatus *md_signer_status_new(void);
void md_signer_status_refresh(MdSignerStatus *self);
const char *md_signer_status_connection(MdSignerStatus *self);
const char *md_signer_status_key(MdSignerStatus *self);
const char *md_signer_status_session(MdSignerStatus *self);
const char *md_signer_status_signing(MdSignerStatus *self);
/* Optional SDK snapshot; NULL when no supported status is available. */
const char *md_signer_status_remote(MdSignerStatus *self);
