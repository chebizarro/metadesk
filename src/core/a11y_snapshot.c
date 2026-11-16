#include "a11y_snapshot.h"
#include <stdlib.h>

struct MdA11ySnapshot {
    MdA11yCtx *a11y;
    MdA11yNode *root;
    pthread_mutex_t mu;
    bool valid;
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
    return snapshot;
}

const MdA11yNode *md_a11y_snapshot_acquire(MdA11ySnapshot *snapshot)
{
    if (!snapshot) return NULL;
    pthread_mutex_lock(&snapshot->mu);
    if (!snapshot->valid || !snapshot->root) {
        md_a11y_node_free(snapshot->root);
        snapshot->root = md_a11y_walk(snapshot->a11y);
        snapshot->valid = snapshot->root != NULL;
    }
    return snapshot->root;
}

void md_a11y_snapshot_release(MdA11ySnapshot *snapshot)
{
    if (snapshot) pthread_mutex_unlock(&snapshot->mu);
}

void md_a11y_snapshot_invalidate(MdA11ySnapshot *snapshot)
{
    if (!snapshot) return;
    pthread_mutex_lock(&snapshot->mu);
    snapshot->valid = false;
    pthread_mutex_unlock(&snapshot->mu);
}

void md_a11y_snapshot_destroy(MdA11ySnapshot *snapshot)
{
    if (!snapshot) return;
    md_a11y_node_free(snapshot->root);
    pthread_mutex_destroy(&snapshot->mu);
    free(snapshot);
}
