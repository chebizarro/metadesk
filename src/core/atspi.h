/*
 * metadesk — atspi.h
 * AT-SPI2 accessibility tree walker and delta generation.
 * See spec §3.3 for tree formats, §10 for agent API.
 */
#ifndef MD_ATSPI_H
#define MD_ATSPI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque tree context */
typedef struct MdAtspiTree MdAtspiTree;

/* Single UI tree node */
typedef struct MdAtspiNode {
    char     *id;       /* stable node identifier, e.g. "node_42" */
    char     *role;     /* AT-SPI2 role string                    */
    char     *label;    /* accessible name/label                  */
    char    **states;   /* NULL-terminated array of state strings  */
    int       state_count;
    int       x, y, w, h;  /* bounding box                       */
    struct MdAtspiNode **children;
    int       child_count;
} MdAtspiNode;

/* Delta operation types — spec §3.3.3 */
typedef enum {
    MD_ATSPI_OP_ADD,
    MD_ATSPI_OP_REMOVE,
    MD_ATSPI_OP_UPDATE,
} MdAtspiOp;

/* A single tree delta entry */
typedef struct {
    MdAtspiOp    op;
    MdAtspiNode *node;
    char        *parent_id; /* for ADD: parent to attach under */
} MdAtspiDelta;

/* Create tree context, connecting to AT-SPI2 bus. */
MdAtspiTree *md_atspi_create(void);

/* Walk the full accessibility tree. Returns root node, caller must free. */
MdAtspiNode *md_atspi_walk(MdAtspiTree *tree);

/* Compute delta between previous and current tree state. */
MdAtspiDelta *md_atspi_diff(MdAtspiTree *tree, int *delta_count);

/* Serialize node tree to full JSON (spec §3.3.1). Caller frees returned string. */
char *md_atspi_to_json(const MdAtspiNode *root);

/* Serialize node tree to compact format (spec §3.3.2). Caller frees. */
char *md_atspi_to_compact(const MdAtspiNode *root);

/* Serialize deltas to JSON. Caller frees. */
char *md_atspi_delta_to_json(const MdAtspiDelta *deltas, int count);

/* Free a node tree recursively. */
void md_atspi_node_free(MdAtspiNode *node);

/* Free delta array. */
void md_atspi_delta_free(MdAtspiDelta *deltas, int count);

/* Destroy tree context. */
void md_atspi_destroy(MdAtspiTree *tree);

#ifdef __cplusplus
}
#endif

#endif /* MD_ATSPI_H */
