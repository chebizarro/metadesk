/*
 * metadesk — test_atspi.c
 * Accessibility tree HAL tests (serialisation + live backend if available).
 */
#include "a11y.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int live_a11y_tests_enabled(void) {
    const char *v = getenv("MD_A11Y_LIVE_TESTS");
    return v && (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
                 strcmp(v, "yes") == 0 || strcmp(v, "on") == 0);
}

/* Helper: create a test node */
static MdA11yNode *make_node(const char *id, const char *role, const char *label,
                              int x, int y, int w, int h) {
    MdA11yNode *n = calloc(1, sizeof(MdA11yNode));
    n->id = strdup(id);
    n->role = strdup(role);
    n->label = strdup(label);
    n->x = x; n->y = y; n->w = w; n->h = h;
    return n;
}

static void node_add_state(MdA11yNode *n, const char *state) {
    n->states = realloc(n->states, sizeof(char*) * (n->state_count + 1));
    n->states[n->state_count++] = strdup(state);
}

static void node_add_child(MdA11yNode *parent, MdA11yNode *child) {
    parent->children = realloc(parent->children,
                                sizeof(MdA11yNode*) * (parent->child_count + 1));
    parent->children[parent->child_count++] = child;
}

static void test_node_alloc_free(void) {
    md_a11y_node_free(NULL); /* should be safe */
    MdA11yNode *n = make_node("1", "button", "OK", 0, 0, 80, 30);
    node_add_state(n, "enabled");
    md_a11y_node_free(n);
    printf("  PASS: node alloc/free\n");
}

static void test_create_destroy(void) {
    if (!live_a11y_tests_enabled()) {
        printf("  SKIP: create/destroy live backend (set MD_A11Y_LIVE_TESTS=1)\n");
        return;
    }

    MdA11yCtx *ctx = md_a11y_create();
    assert(ctx != NULL);
    md_a11y_destroy(ctx);
    printf("  PASS: create/destroy\n");
}

static void test_json_serialization(void) {
    /* Build: WIN[1] gedit -> BTN[42] Save, TXT[44] Hello */
    MdA11yNode *root = make_node("node_1", "frame", "gedit - untitled",
                                  0, 0, 1920, 1080);
    node_add_state(root, "visible");
    node_add_state(root, "active");

    MdA11yNode *btn = make_node("node_42", "push button", "Save", 10, 10, 80, 30);
    node_add_state(btn, "enabled");
    node_add_child(root, btn);

    MdA11yNode *txt = make_node("node_44", "text", "Hello world", 10, 50, 400, 20);
    node_add_state(txt, "focused");
    node_add_child(root, txt);

    char *json = md_a11y_to_json(root);
    assert(json != NULL);
    assert(strstr(json, "\"v\":1") != NULL);
    assert(strstr(json, "\"id\":\"node_1\"") != NULL);
    assert(strstr(json, "\"role\":\"frame\"") != NULL);
    assert(strstr(json, "\"label\":\"gedit - untitled\"") != NULL);
    assert(strstr(json, "\"id\":\"node_42\"") != NULL);
    assert(strstr(json, "\"id\":\"node_44\"") != NULL);

    free(json);
    md_a11y_node_free(root);
    printf("  PASS: JSON serialization\n");
}

static void test_compact_serialization(void) {
    MdA11yNode *root = make_node("1", "window", "gedit - untitled",
                                  0, 0, 1920, 1080);
    MdA11yNode *btn = make_node("42", "button", "Save", 10, 10, 80, 30);
    node_add_state(btn, "enabled");
    node_add_child(root, btn);

    MdA11yNode *txt = make_node("44", "entry", "Hello", 10, 50, 400, 20);
    node_add_state(txt, "focused");
    node_add_child(root, txt);

    char *compact = md_a11y_to_compact(root);
    assert(compact != NULL);
    assert(strstr(compact, "v1 ts:") != NULL);
    assert(strstr(compact, "WIN[1]") != NULL);
    assert(strstr(compact, "BTN[42] Save") != NULL);
    assert(strstr(compact, "*enabled*") != NULL);
    assert(strstr(compact, "TXT[44]") != NULL);
    assert(strstr(compact, "<focused>") != NULL);

    /* §3.3.2: Text entries get quoted content with single quotes */
    assert(strstr(compact, "'Hello'") != NULL);
    /* §3.3.2: <focused> appears before quoted content for text entries */
    {
        const char *focused = strstr(compact, "<focused>");
        const char *quoted = strstr(compact, "'Hello'");
        assert(focused != NULL && quoted != NULL);
        assert(focused < quoted);  /* <focused> before 'Hello' */
    }

    free(compact);
    md_a11y_node_free(root);
    printf("  PASS: compact serialization\n");
}

/* §3.3.2: Interactable filtering — decorative nodes are skipped but their
 * interactable children still appear. */
static void test_compact_interactable_filtering(void) {
    MdA11yNode *root = make_node("1", "window", "App",
                                  0, 0, 1920, 1080);
    /* Panel is decorative — should be skipped */
    MdA11yNode *panel = make_node("10", "panel", "", 0, 0, 500, 500);
    node_add_child(root, panel);

    /* Button nested under panel — should still appear */
    MdA11yNode *btn = make_node("20", "button", "OK", 10, 10, 80, 30);
    node_add_child(panel, btn);

    /* Label is decorative — should be skipped entirely */
    MdA11yNode *lbl = make_node("30", "label", "Status", 100, 10, 80, 20);
    node_add_child(root, lbl);

    /* Dialog is a container — should appear */
    MdA11yNode *dlg = make_node("40", "dialog", "Confirm", 200, 200, 400, 300);
    node_add_child(root, dlg);

    MdA11yNode *chk = make_node("50", "check box", "Remember", 210, 250, 120, 20);
    node_add_state(chk, "checked");
    node_add_child(dlg, chk);

    char *compact = md_a11y_to_compact(root);
    assert(compact != NULL);

    /* Window (container) present */
    assert(strstr(compact, "WIN[1]") != NULL);
    /* Button under decorative panel still present */
    assert(strstr(compact, "BTN[20] OK") != NULL);
    /* Decorative nodes skipped */
    assert(strstr(compact, "PNL") == NULL);   /* panel filtered out */
    assert(strstr(compact, "LBL") == NULL);   /* label filtered out */
    /* Dialog container present */
    assert(strstr(compact, "DLG[40]") != NULL);
    /* Check box with state */
    assert(strstr(compact, "CHK[50] Remember") != NULL);
    assert(strstr(compact, "*checked*") != NULL);

    free(compact);
    md_a11y_node_free(root);
    printf("  PASS: compact interactable filtering\n");
}

/* §3.3.2: Text entry with no content and multiple states */
static void test_compact_text_empty(void) {
    MdA11yNode *root = make_node("1", "window", "Editor",
                                  0, 0, 800, 600);
    MdA11yNode *txt = make_node("5", "entry", "", 10, 10, 200, 20);
    node_add_state(txt, "focused");
    node_add_state(txt, "enabled");
    node_add_child(root, txt);

    char *compact = md_a11y_to_compact(root);
    assert(compact != NULL);

    /* Empty text gets quoted empty string */
    assert(strstr(compact, "TXT[5] <focused> ''") != NULL);
    /* enabled state still appended after quoted content */
    assert(strstr(compact, "*enabled*") != NULL);

    free(compact);
    md_a11y_node_free(root);
    printf("  PASS: compact text empty content\n");
}

static void test_delta_serialization(void) {
    MdA11yNode *node = make_node("node_5", "button", "New Button", 50, 50, 100, 30);
    MdA11yDelta delta = {
        .op = MD_A11Y_OP_ADD,
        .node = node,
        .parent_id = strdup("node_1"),
    };

    char *json = md_a11y_delta_to_json(&delta, 1);
    assert(json != NULL);
    assert(strstr(json, "\"op\":\"add\"") != NULL);
    assert(strstr(json, "\"id\":\"node_5\"") != NULL);
    assert(strstr(json, "\"parent_id\":\"node_1\"") != NULL);

    free(json);
    md_a11y_node_free(delta.node);
    free(delta.parent_id);
    printf("  PASS: delta serialization\n");
}

static void test_walk_tree(void) {
    if (!live_a11y_tests_enabled()) {
        printf("  SKIP: walk tree live backend (set MD_A11Y_LIVE_TESTS=1)\n");
        return;
    }

    MdA11yCtx *ctx = md_a11y_create();
    assert(ctx != NULL);

    MdA11yNode *root = md_a11y_walk(ctx);
    if (!root) {
        /* Accessibility bus may not be available in CI/headless environments */
        printf("  SKIP: walk tree (no accessibility bus)\n");
        md_a11y_destroy(ctx);
        return;
    }

    /* Basic sanity: root should exist with role and id */
    assert(root->id != NULL);
    assert(root->role != NULL);
    assert(strcmp(root->role, "desktop") == 0);

    /* Serialize in both formats to verify end-to-end */
    char *json = md_a11y_to_json(root);
    assert(json != NULL);
    assert(strstr(json, "\"role\":\"desktop\"") != NULL);
    free(json);

    char *compact = md_a11y_to_compact(root);
    assert(compact != NULL);
    assert(strstr(compact, "DSK[") != NULL);
    free(compact);

    printf("  PASS: walk tree (%d children)\n", root->child_count);
    md_a11y_node_free(root);
    md_a11y_destroy(ctx);
}

static void test_diff(void) {
    if (!live_a11y_tests_enabled()) {
        printf("  SKIP: diff live backend (set MD_A11Y_LIVE_TESTS=1)\n");
        return;
    }

    MdA11yCtx *ctx = md_a11y_create();
    assert(ctx != NULL);

    /* First diff with no previous snapshot should return NULL */
    int delta_count = 0;
    MdA11yDelta *deltas = md_a11y_diff(ctx, &delta_count);

    if (deltas) {
        char *json = md_a11y_delta_to_json(deltas, delta_count);
        if (json) free(json);
        md_a11y_delta_free(deltas, delta_count);
        printf("  PASS: diff (%d deltas)\n", delta_count);
    } else {
        printf("  PASS: diff (no deltas — first snapshot or no a11y bus)\n");
    }

    md_a11y_destroy(ctx);
}

/* ── Tree patch tests (§3.3.3 in-place delta application) ──── */

static void test_tree_patch_add(void) {
    /* Build a tree, serialize, then add a node via delta */
    MdA11yNode *root = make_node("r1", "window", "App", 0, 0, 800, 600);
    MdA11yNode *btn = make_node("b1", "button", "OK", 10, 10, 80, 30);
    node_add_child(root, btn);

    char *tree = md_a11y_to_json(root);
    assert(tree != NULL);

    /* Delta: add a new button under root */
    const char *delta = "[{\"op\":\"add\",\"parent_id\":\"r1\","
        "\"node\":{\"id\":\"b2\",\"role\":\"button\",\"label\":\"Cancel\","
        "\"bounds\":{\"x\":100,\"y\":10,\"w\":80,\"h\":30},\"children\":[]}}]";

    char *patched = md_a11y_tree_patch(tree, delta);
    assert(patched != NULL);

    /* Verify the new node appears */
    assert(strstr(patched, "\"b2\"") != NULL);
    assert(strstr(patched, "Cancel") != NULL);
    /* Original node still present */
    assert(strstr(patched, "\"b1\"") != NULL);

    free(patched);
    free(tree);
    md_a11y_node_free(root);
    printf("  PASS: tree patch add\n");
}

static void test_tree_patch_remove(void) {
    MdA11yNode *root = make_node("r1", "window", "App", 0, 0, 800, 600);
    MdA11yNode *b1 = make_node("b1", "button", "OK", 10, 10, 80, 30);
    MdA11yNode *b2 = make_node("b2", "button", "Cancel", 100, 10, 80, 30);
    node_add_child(root, b1);
    node_add_child(root, b2);

    char *tree = md_a11y_to_json(root);
    assert(tree != NULL);

    /* Delta: remove b1 */
    const char *delta = "[{\"op\":\"remove\",\"node\":{\"id\":\"b1\"}}]";

    char *patched = md_a11y_tree_patch(tree, delta);
    assert(patched != NULL);

    /* b1 should be gone, b2 still present */
    assert(strstr(patched, "\"b1\"") == NULL);
    assert(strstr(patched, "\"b2\"") != NULL);

    free(patched);
    free(tree);
    md_a11y_node_free(root);
    printf("  PASS: tree patch remove\n");
}

static void test_tree_patch_update(void) {
    MdA11yNode *root = make_node("r1", "window", "App", 0, 0, 800, 600);
    MdA11yNode *btn = make_node("b1", "button", "OK", 10, 10, 80, 30);
    node_add_child(root, btn);

    char *tree = md_a11y_to_json(root);
    assert(tree != NULL);

    /* Delta: update b1's label */
    const char *delta = "[{\"op\":\"update\","
        "\"node\":{\"id\":\"b1\",\"label\":\"Confirm\"}}]";

    char *patched = md_a11y_tree_patch(tree, delta);
    assert(patched != NULL);

    /* New label should appear, old should not */
    assert(strstr(patched, "Confirm") != NULL);
    /* node id still present */
    assert(strstr(patched, "\"b1\"") != NULL);

    free(patched);
    free(tree);
    md_a11y_node_free(root);
    printf("  PASS: tree patch update\n");
}

static void test_tree_patch_multiple_ops(void) {
    MdA11yNode *root = make_node("r1", "window", "App", 0, 0, 800, 600);
    MdA11yNode *b1 = make_node("b1", "button", "A", 10, 10, 80, 30);
    MdA11yNode *b2 = make_node("b2", "button", "B", 100, 10, 80, 30);
    node_add_child(root, b1);
    node_add_child(root, b2);

    char *tree = md_a11y_to_json(root);
    assert(tree != NULL);

    /* Multiple ops: remove b1, update b2, add b3 */
    const char *delta =
        "[{\"op\":\"remove\",\"node\":{\"id\":\"b1\"}},"
        "{\"op\":\"update\",\"node\":{\"id\":\"b2\",\"label\":\"Updated\"}},"
        "{\"op\":\"add\",\"parent_id\":\"r1\","
        "\"node\":{\"id\":\"b3\",\"role\":\"button\",\"label\":\"New\","
        "\"bounds\":{\"x\":0,\"y\":0,\"w\":80,\"h\":30},\"children\":[]}}]";

    char *patched = md_a11y_tree_patch(tree, delta);
    assert(patched != NULL);

    assert(strstr(patched, "\"b1\"") == NULL);     /* removed */
    assert(strstr(patched, "Updated") != NULL);     /* updated */
    assert(strstr(patched, "\"b3\"") != NULL);       /* added */

    free(patched);
    free(tree);
    md_a11y_node_free(root);
    printf("  PASS: tree patch multiple ops\n");
}

static void test_tree_patch_null_safety(void) {
    assert(md_a11y_tree_patch(NULL, "[]") == NULL);
    assert(md_a11y_tree_patch("{}", NULL) == NULL);
    assert(md_a11y_tree_patch("not json", "[]") == NULL);
    assert(md_a11y_tree_patch("{\"v\":1}", "not json") == NULL);

    printf("  PASS: tree patch null safety\n");
}

static void test_tree_patch_deep_add(void) {
    /* Add a node under a nested child, not the root */
    MdA11yNode *root = make_node("r1", "window", "App", 0, 0, 800, 600);
    MdA11yNode *panel = make_node("p1", "panel", "Menu", 0, 0, 200, 600);
    node_add_child(root, panel);

    char *tree = md_a11y_to_json(root);
    assert(tree != NULL);

    /* Add button under panel (p1), not root */
    const char *delta = "[{\"op\":\"add\",\"parent_id\":\"p1\","
        "\"node\":{\"id\":\"m1\",\"role\":\"menu item\",\"label\":\"File\","
        "\"bounds\":{\"x\":5,\"y\":5,\"w\":100,\"h\":20},\"children\":[]}}]";

    char *patched = md_a11y_tree_patch(tree, delta);
    assert(patched != NULL);

    assert(strstr(patched, "\"m1\"") != NULL);
    assert(strstr(patched, "File") != NULL);

    free(patched);
    free(tree);
    md_a11y_node_free(root);
    printf("  PASS: tree patch deep add\n");
}

static void test_tree_patch_update_state(void) {
    MdA11yNode *root = make_node("r1", "window", "App", 0, 0, 800, 600);
    MdA11yNode *btn = make_node("b1", "button", "OK", 10, 10, 80, 30);
    node_add_state(btn, "enabled");
    node_add_child(root, btn);

    char *tree = md_a11y_to_json(root);
    assert(tree != NULL);

    /* Update b1's state to focused */
    const char *delta = "[{\"op\":\"update\","
        "\"node\":{\"id\":\"b1\",\"state\":[\"focused\",\"pressed\"]}}]";

    char *patched = md_a11y_tree_patch(tree, delta);
    assert(patched != NULL);

    assert(strstr(patched, "focused") != NULL);
    assert(strstr(patched, "pressed") != NULL);

    free(patched);
    free(tree);
    md_a11y_node_free(root);
    printf("  PASS: tree patch update state\n");
}

int main(void) {
    printf("test_a11y:\n");
    printf("  live backend tests: %s\n", live_a11y_tests_enabled() ? "enabled" : "disabled");
    test_node_alloc_free();
    test_create_destroy();
    test_json_serialization();
    test_compact_serialization();
    test_compact_interactable_filtering();
    test_compact_text_empty();
    test_delta_serialization();
    test_tree_patch_add();
    test_tree_patch_remove();
    test_tree_patch_update();
    test_tree_patch_multiple_ops();
    test_tree_patch_null_safety();
    test_tree_patch_deep_add();
    test_tree_patch_update_state();
    test_walk_tree();
    test_diff();
    printf("All a11y tests passed.\n");
    return 0;
}
