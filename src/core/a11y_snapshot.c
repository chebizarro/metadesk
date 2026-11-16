#include "a11y_snapshot.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <time.h>

struct MdA11ySnapshot {
    MdA11yCtx *a11y;
    MdA11yNode *root;
    pthread_mutex_t mu;
    atomic_bool stale;
    bool caching;
    uint64_t time_ms;
};

MdA11ySnapshot *md_a11y_snapshot_create(MdA11yCtx *a11y)
{
    if (!a11y) return NULL;
    MdA11ySnapshot *snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) return NULL;
    if (pthread_mutex_init(&snapshot->mu, NULL) != 0) {
        free(snapshot);
        return NULL;
    }
    snapshot->a11y = a11y;
    atomic_init(&snapshot->stale, true);
    snapshot->caching = true;
    return snapshot;
}

const MdA11yNode *md_a11y_snapshot_acquire(MdA11ySnapshot *snapshot)
{
    if (!snapshot) return NULL;
    pthread_mutex_lock(&snapshot->mu);
    /* Clear before walking so an event during the walk remains pending. */
    bool stale = atomic_exchange(&snapshot->stale, false);
    if (!snapshot->caching || stale || !snapshot->root) {
        md_a11y_node_free(snapshot->root);
        snapshot->root = md_a11y_walk(snapshot->a11y);
        snapshot->time_ms = 0;
        if (snapshot->root) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            snapshot->time_ms = (uint64_t)ts.tv_sec * 1000 +
                                (uint64_t)(ts.tv_nsec / 1000000);
        }
    }
    return snapshot->root;
}

void md_a11y_snapshot_release(MdA11ySnapshot *snapshot)
{
    if (snapshot) pthread_mutex_unlock(&snapshot->mu);
}

void md_a11y_snapshot_invalidate(MdA11ySnapshot *snapshot)
{
    if (snapshot) atomic_store(&snapshot->stale, true);
}

void md_a11y_snapshot_set_caching(MdA11ySnapshot *snapshot, bool enabled)
{
    if (!snapshot) return;
    pthread_mutex_lock(&snapshot->mu);
    snapshot->caching = enabled;
    atomic_store(&snapshot->stale, true);
    pthread_mutex_unlock(&snapshot->mu);
}

uint64_t md_a11y_snapshot_time_ms(const MdA11ySnapshot *snapshot)
{
    return snapshot ? snapshot->time_ms : 0;
}

void md_a11y_snapshot_destroy(MdA11ySnapshot *snapshot)
{
    if (!snapshot) return;
    md_a11y_node_free(snapshot->root);
    pthread_mutex_destroy(&snapshot->mu);
    free(snapshot);
}
