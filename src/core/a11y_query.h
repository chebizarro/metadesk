/* Platform-neutral queries over borrowed accessibility snapshots. */
#ifndef MD_A11Y_QUERY_H
#define MD_A11Y_QUERY_H

#include "a11y.h"

#ifdef __cplusplus
extern "C" {
#endif

const MdA11yNode *md_a11y_find_node_by_id(const MdA11yNode *root, const char *id);
bool md_a11y_matches(const MdA11yNode *node, const char *role,
                     const char *label_contains, const char *state);
bool md_a11y_is_window(const MdA11yNode *node);
/* First focused node and its nearest frame/window/dialog/alert ancestor. */
const MdA11yNode *md_a11y_find_focused(const MdA11yNode *root,
                                      const MdA11yNode **window);
/* Collect matches in tree order, up to limit. Output pointers are borrowed. */
int md_a11y_find(const MdA11yNode *root, const char *role,
                 const char *label_contains, const char *state,
                 const MdA11yNode **out, int limit);
/* Top-level frames/windows plus dialogs/alerts, including nested modals. */
void md_a11y_visit_windows(const MdA11yNode *root,
                            void (*visit)(const MdA11yNode *, void *),
                            void *userdata);

#ifdef __cplusplus
}
#endif
#endif
