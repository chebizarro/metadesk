/* Platform-neutral queries over MdA11yNode snapshots. */
#include "a11y_query.h"
#include <ctype.h>
#include <string.h>

/* Keep case folding portable to MSVC (no strcasecmp/strcasestr). */
static bool equal_ci(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++))
            return false;
    }
    return *a == *b;
}

static bool contains_ci(const char *text, const char *part)
{
    if (!*part) return true;
    if (!text) return false;
    for (; *text; text++) {
        const char *a = text, *b = part;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b) return true;
    }
    return false;
}

const MdA11yNode *md_a11y_find_node_by_id(const MdA11yNode *node, const char *id)
{
    if (!node || !id) return NULL;
    if (node->id && strcmp(node->id, id) == 0) return node;
    for (int i = 0; node->children && i < node->child_count; i++) {
        const MdA11yNode *found = md_a11y_find_node_by_id(node->children[i], id);
        if (found) return found;
    }
    return NULL;
}

bool md_a11y_matches(const MdA11yNode *node, const char *role,
                     const char *label_contains, const char *state)
{
    if (!node || (role && !equal_ci(node->role, role)) ||
        (label_contains && !contains_ci(node->label, label_contains)))
        return false;
    if (!state) return true;
    for (int i = 0; node->states && i < node->state_count; i++)
        if (equal_ci(node->states[i], state)) return true;
    return false;
}

static bool is_modal(const MdA11yNode *node)
{
    return node && (equal_ci(node->role, "dialog") || equal_ci(node->role, "alert"));
}

bool md_a11y_is_window(const MdA11yNode *node)
{
    return node && (equal_ci(node->role, "frame") || equal_ci(node->role, "window") ||
                    is_modal(node));
}

static const MdA11yNode *focused(const MdA11yNode *node,
                                const MdA11yNode *ancestor,
                                const MdA11yNode **window)
{
    if (!node) return NULL;
    if (md_a11y_matches(node, NULL, NULL, "focused")) {
        *window = ancestor;
        return node;
    }
    if (md_a11y_is_window(node)) ancestor = node;
    for (int i = 0; node->children && i < node->child_count; i++) {
        const MdA11yNode *found = focused(node->children[i], ancestor, window);
        if (found) return found;
    }
    return NULL;
}

const MdA11yNode *md_a11y_find_focused(const MdA11yNode *root,
                                      const MdA11yNode **window)
{
    *window = NULL;
    return focused(root, NULL, window);
}

int md_a11y_find(const MdA11yNode *root, const char *role,
                 const char *label_contains, const char *state,
                 const MdA11yNode **out, int limit)
{
    if (!root || limit <= 0) return 0;
    int count = 0;
    if (md_a11y_matches(root, role, label_contains, state)) out[count++] = root;
    for (int i = 0; root->children && i < root->child_count && count < limit; i++)
        count += md_a11y_find(root->children[i], role, label_contains, state,
                              out + count, limit - count);
    return count;
}

static void visit_windows(const MdA11yNode *root, bool in_window,
                           void (*visit)(const MdA11yNode *, void *),
                           void *userdata)
{
    if (!root) return;
    if (md_a11y_is_window(root)) {
        if (!in_window || is_modal(root)) visit(root, userdata);
        in_window = true;
    }
    for (int i = 0; root->children && i < root->child_count; i++)
        visit_windows(root->children[i], in_window, visit, userdata);
}

void md_a11y_visit_windows(const MdA11yNode *root,
                            void (*visit)(const MdA11yNode *, void *),
                            void *userdata)
{
    visit_windows(root, false, visit, userdata);
}
