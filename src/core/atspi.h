/*
 * metadesk — atspi.h
 * COMPATIBILITY HEADER — redirects to a11y.h
 *
 * The accessibility tree interface has been generalised to support
 * multiple platforms. Use a11y.h directly for new code.
 *
 * Type mapping:
 *   MdAtspiTree  → MdA11yCtx
 *   MdAtspiNode  → MdA11yNode
 *   MdAtspiDelta → MdA11yDelta
 *   MdAtspiOp    → MdA11yOp
 */
#ifndef MD_ATSPI_H
#define MD_ATSPI_H

#include "a11y.h"

/* Legacy type aliases */
typedef MdA11yCtx    MdAtspiTree;
typedef MdA11yNode   MdAtspiNode;
typedef MdA11yDelta  MdAtspiDelta;

#define MD_ATSPI_OP_ADD    MD_A11Y_OP_ADD
#define MD_ATSPI_OP_REMOVE MD_A11Y_OP_REMOVE
#define MD_ATSPI_OP_UPDATE MD_A11Y_OP_UPDATE

/* Legacy function aliases */
#define md_atspi_create()          md_a11y_create()
#define md_atspi_walk(t)           md_a11y_walk(t)
#define md_atspi_diff(t, c)        md_a11y_diff((t), (c))
#define md_atspi_to_json(r)        md_a11y_to_json(r)
#define md_atspi_to_compact(r)     md_a11y_to_compact(r)
#define md_atspi_delta_to_json(d,c) md_a11y_delta_to_json((d),(c))
#define md_atspi_node_free(n)      md_a11y_node_free(n)
#define md_atspi_delta_free(d,c)   md_a11y_delta_free((d),(c))
#define md_atspi_destroy(t)        md_a11y_destroy(t)

#endif /* MD_ATSPI_H */
