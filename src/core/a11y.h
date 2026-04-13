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

#include <stdint.h>
#include <stdbool.h>

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

typedef void (*MdA11yChangeCb)(const MdA11yDelta *deltas, int count,
                               void *userdata);

/* ── Backend vtable (spec §2.3.2) ────────────────────────────── */

typedef struct MdA11yBackend {
    int   (*init)(MdA11yCtx *ctx);
    int   (*get_tree)(MdA11yCtx *ctx, MdA11yNode **out_root);
    int   (*get_diff)(MdA11yCtx *ctx, MdA11yDelta **out_deltas, int *out_count);
    int   (*subscribe_changes)(MdA11yCtx *ctx, MdA11yChangeCb cb, void *userdata);
    void  (*destroy)(MdA11yCtx *ctx);
} MdA11yBackend;

/* ── A11y context ────────────────────────────────────────────── */

struct MdA11yCtx {
    const MdA11yBackend *vtable;
    void                *backend_data;  /* backend-private state */
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

/* Compute delta between previous and current tree state.
 * Returns delta array (caller frees with md_a11y_delta_free()).
 * *delta_count is set to the number of entries. Returns NULL if
 * no changes or no previous snapshot exists. */
MdA11yDelta *md_a11y_diff(MdA11yCtx *ctx, int *delta_count);

/* Subscribe to live accessibility tree changes.
 * Returns 0 on success, -1 if not supported by the backend. */
int md_a11y_subscribe_changes(MdA11yCtx *ctx, MdA11yChangeCb cb, void *userdata);

/* ── Serialization (spec §3.3) ───────────────────────────────── */

/* Serialize node tree to full JSON (spec §3.3.1). Caller frees. */
char *md_a11y_to_json(const MdA11yNode *root);

/* Serialize node tree to compact format (spec §3.3.2). Caller frees. */
char *md_a11y_to_compact(const MdA11yNode *root);

/* Serialize deltas to JSON (spec §3.3.3). Caller frees. */
char *md_a11y_delta_to_json(const MdA11yDelta *deltas, int count);

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
