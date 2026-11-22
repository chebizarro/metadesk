/*
 * metadesk — a11y.h
 * Platform-agnostic accessibility tree HAL.
 * See spec §2.3.2 — platform backends selected at compile time.
 *
 * Backends:
 *   Linux:   AT-SPI2 via D-Bus    (a11y_atspi.c)
 *   macOS:   AXUIElement          (a11y_axui.m)
 *   Windows: UI Automation        (a11y_uia.cpp)
 *
 * No platform-specific headers appear in this file.
 */
#ifndef MD_A11Y_H
#define MD_A11Y_H

#include "a11y_vocab.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Forward declarations ────────────────────────────────────── */

typedef struct MdA11yCtx MdA11yCtx;

/* ── Tree node (platform-neutral) ────────────────────────────── */

typedef struct MdA11yNode {
    char     *id;           /* stable node identifier, e.g. "n42"        */
    char     *role;         /* role string (platform-normalised)          */
    char     *label;        /* accessible name/label                     */
    char    **states;       /* NULL-terminated array of state strings     */
    int       state_count;
    int       x, y, w, h;  /* bounding box (screen coordinates)          */
    struct MdA11yNode **children;
    int       child_count;
} MdA11yNode;

/* ── Tree delta ──────────────────────────────────────────────── */

typedef enum {
    MD_A11Y_OP_ADD,
    MD_A11Y_OP_REMOVE,
    MD_A11Y_OP_UPDATE,
} MdA11yOp;

typedef struct {
    MdA11yOp    op;
    MdA11yNode *node;
    char       *parent_id;  /* for ADD: parent to attach under */
} MdA11yDelta;

/* ── Change subscription callback ────────────────────────────── */

/* Change hint, not a delta feed: deltas may be NULL and count zero.
 * Consumers needing content must pull via md_a11y_walk(), md_a11y_diff(),
 * or the MCP snapshot. */
typedef void (*MdA11yChangeCb)(const MdA11yDelta *deltas, int count,
                               void *userdata);

/* ── Backend vtable (spec §2.3.2) ────────────────────────────── */

typedef struct MdA11yBackend {
    int   (*init)(MdA11yCtx *ctx);
    int   (*get_tree)(MdA11yCtx *ctx, MdA11yNode **out_root);
    /* Register OS-level change listeners. The callback itself is stored on
     * the MdA11yCtx by a11y.c before this is called; backends must not keep
     * their own copy — they dispatch through md_a11y_change_dispatch(). */
    int   (*subscribe_changes)(MdA11yCtx *ctx, MdA11yChangeCb cb, void *userdata);
    /* Tear down the OS-level listeners registered by subscribe_changes and
     * stop/join any dispatch thread the backend owns. Called by a11y.c only
     * after the callback has been cleared and in-flight dispatches drained,
     * so the backend never has to touch the user callback here. Must be
     * idempotent and safe when nothing is subscribed. */
    void  (*unsubscribe_changes)(MdA11yCtx *ctx);
    void  (*destroy)(MdA11yCtx *ctx);
    /* Unlike capture's active / input's ready, a11y reports backend liveness.
     * Optional for fixture/legacy backends: NULL uses backend_data != NULL. */
    bool  (*is_connected)(const MdA11yCtx *ctx);
    /* Global, z-order-aware native hit test. Return an owned stable node id,
     * or NULL if the platform cannot establish the hit. Never enumerate
     * overlapping windows and guess. Coordinates are global desktop units. */
    char *(*hit_test)(MdA11yCtx *ctx, int x, int y);
    /* Optional semantic primary click. Must revalidate the native hit and
     * activate that exact element; failure must never fall back to input. */
    int (*click_target)(MdA11yCtx *ctx, const char *id, int x, int y);
} MdA11yBackend;

/* ── A11y context ────────────────────────────────────────────── */

struct MdA11yCtx {
    const MdA11yBackend *vtable;
    void                *backend_data;  /* backend-private state */

    /* Previous tree snapshot for delta computation. The diff engine is
     * implemented once in a11y.c on top of get_tree — it operates only
     * on the platform-neutral MdA11yNode, so it does not belong in the
     * three backends. */
    MdA11yNode          *last_snapshot;
    pthread_mutex_t      snapshot_mu;

    /* Live change-callback registration and in-flight guard, owned by
     * a11y.c. Backends never store the callback themselves: every dispatch
     * uses md_a11y_change_dispatch() so that
     * md_a11y_unsubscribe_changes() can clear the registration and wait for
     * a dispatch that already copied it to finish. */
    pthread_mutex_t      change_mu;
    pthread_cond_t       change_cv;
    MdA11yChangeCb       change_cb;
    void                *change_userdata;
    int                  change_in_flight;
};

/* ── Factory ─────────────────────────────────────────────────── */

/* Create the platform-appropriate a11y backend.
 * Each platform's backend source file implements this function. */
const MdA11yBackend *md_a11y_backend_create(void);

/* ── Public convenience API ──────────────────────────────────── */

/* Create and initialise an a11y context with the platform backend.
 * Returns NULL on failure (e.g. accessibility service unavailable). */
MdA11yCtx *md_a11y_create(void);

/* Walk the full accessibility tree. Returns root node; caller must
 * free with md_a11y_node_free(). Returns NULL on failure. */
MdA11yNode *md_a11y_walk(MdA11yCtx *ctx);

/* Fail closed if a native hit test cannot identify this exact target. */
bool md_a11y_target_at_point(MdA11yCtx *ctx, const char *id, int x, int y);

/* Compute delta between previous and current tree state.
 * Returns delta array (caller frees with md_a11y_delta_free()).
 * *delta_count is set to the number of entries. Returns NULL if
 * no changes or no previous snapshot exists. */
MdA11yDelta *md_a11y_diff(MdA11yCtx *ctx, int *delta_count);

/* Subscribe to live accessibility tree changes.
 * Returns a non-negative backend handle (or 0 if no fd is exposed) on success,
 * -1 if not supported by the backend. The callback is invoked from a
 * backend-owned dispatch thread, never from inside this call.
 *
 * A context holds at most one subscription. Subscribing while a different
 * (cb, userdata) pair is registered fails with -1 and leaves the existing
 * subscription untouched; re-subscribing the identical pair is a no-op
 * success. Callers must therefore only unsubscribe what they subscribed. */
int md_a11y_subscribe_changes(MdA11yCtx *ctx, MdA11yChangeCb cb, void *userdata);

/* Detach the change callback registered with md_a11y_subscribe_changes().
 *
 * Drain guarantee: when this returns, the callback is not running on any
 * thread and will never be invoked again, so the caller may free whatever
 * `userdata` points at. Internally the registration is cleared first, then
 * the call blocks until every dispatch that had already copied the callback
 * has returned, and only then are the backend's OS listeners and dispatch
 * thread torn down.
 *
 * Must NOT be called from inside the change callback itself: the caller
 * would be waiting for its own invocation to finish. (That case is detected
 * per thread, logged, and does not deadlock — the drain still waits for
 * every *other* in-flight dispatch — but the guarantee cannot hold for the
 * caller's own invocation.) Do not call while holding any lock the callback
 * may take.
 *
 * Returns 0 on success, -1 if nothing was subscribed or the backend cannot
 * detach its listeners (the callback is still cleared and drained in the
 * latter case, so it stays safe to free userdata). */
int md_a11y_unsubscribe_changes(MdA11yCtx *ctx);

/* Dispatch a hint without walking the tree; participates in unsubscribe drain. */
void md_a11y_change_dispatch(MdA11yCtx *ctx);

/* ── Serialization (spec §3.3) ───────────────────────────────── */

/* Serialize node tree to full JSON (spec §3.3.1). Caller frees. */
char *md_a11y_to_json(const MdA11yNode *root);

/* Serialize node tree to compact format (spec §3.3.2). Caller frees. */
char *md_a11y_to_compact(const MdA11yNode *root);

/* Full-tree serializers with the snapshot's wall-clock timestamp.
 * The non-_at variants above use the current time. Caller frees. */
char *md_a11y_to_json_at(const MdA11yNode *root, uint64_t ts_ms);
char *md_a11y_to_compact_at(const MdA11yNode *root, uint64_t ts_ms);

/* Targeted serializers: depth 0 includes only root; -1 is unbounded.
 * Unlike the full compact tree, targeted compact output includes every role.
 * Caller owns the returned JSON object / string. */
cJSON *md_a11y_node_to_json(const MdA11yNode *node, int depth);
char *md_a11y_query_to_compact(const MdA11yNode *node, int depth);
/* Serialize matches into one compact buffer, without descendants.
 * Returns an allocated empty string for zero matches. Caller frees. */
char *md_a11y_matches_to_compact(const MdA11yNode *const *nodes, int count);

/* Serialize deltas to JSON (spec §3.3.3). Caller frees. */
char *md_a11y_delta_to_json(const MdA11yDelta *deltas, int count);

/*
 * Apply a delta JSON array to an existing tree JSON document in-place.
 *
 * tree_json: full tree JSON string ({"v":1,"ts":...,"root":{...}})
 * delta_json: delta array ([{"op":"add/remove/update","node":{...},...}])
 *
 * Returns a newly-allocated JSON string with the patched tree.
 * The timestamp ("ts") is updated. Caller frees the result.
 * Returns NULL on error (invalid JSON, missing fields, etc.).
 *
 * Operations:
 *   add:    insert node under parent_id (appended to children)
 *   remove: delete node by id (and all its descendants)
 *   update: replace node fields (id must match an existing node)
 */
char *md_a11y_tree_patch(const char *tree_json, const char *delta_json);

/* ── Memory management ───────────────────────────────────────── */

/* Free a node tree recursively. */
void md_a11y_node_free(MdA11yNode *node);

/* Free delta array. */
void md_a11y_delta_free(MdA11yDelta *deltas, int count);

/* Query whether the a11y context is connected and operational. */
bool md_a11y_is_connected(const MdA11yCtx *ctx);

/* Destroy a11y context, freeing all resources. */
void md_a11y_destroy(MdA11yCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MD_A11Y_H */
