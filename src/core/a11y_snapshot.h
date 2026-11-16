/* Bridge-owned, lazy accessibility snapshot; independent of diff state. */
#ifndef MD_A11Y_SNAPSHOT_H
#define MD_A11Y_SNAPSHOT_H

#include "a11y.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdA11ySnapshot MdA11ySnapshot;

/* Borrows a11y, which must outlive the snapshot. */
MdA11ySnapshot *md_a11y_snapshot_create(MdA11yCtx *a11y);
/* Holds the snapshot lock until release, even when returning NULL.
 * The returned tree is borrowed and must not be modified or freed. */
const MdA11yNode *md_a11y_snapshot_acquire(MdA11ySnapshot *snapshot);
void md_a11y_snapshot_release(MdA11ySnapshot *snapshot);
/* Marks stale without taking the read lock; the next acquire refreshes.
 * Safe from a change callback during a walk/read. NULL is a no-op. */
void md_a11y_snapshot_invalidate(MdA11ySnapshot *snapshot);
/* Disable caching when change events are unavailable: every acquire walks. */
void md_a11y_snapshot_set_caching(MdA11ySnapshot *snapshot, bool enabled);
/* Successful walk's wall-clock time, or 0 if unavailable.
 * Caller must hold the acquire lock until this returns. */
uint64_t md_a11y_snapshot_time_ms(const MdA11ySnapshot *snapshot);
/* Call only after readers and change callbacks have stopped. */
void md_a11y_snapshot_destroy(MdA11ySnapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif
