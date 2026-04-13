/*
 * metadesk — test_atspi.c
 * AT-SPI2 tree serialisation tests.
 */
#include "atspi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Helper: create a test node */
static MdAtspiNode *make_node(const char *id, const char *role, const char *label,
                               int x, int y, int w, int h) {
    MdAtspiNode *n = calloc(1, sizeof(MdAtspiNode));
    n->id = strdup(id);
    n->role = strdup(role);
    n->label = strdup(label);
    n->x = x; n->y = y; n->w = w; n->h = h;
    return n;
}

static void node_add_state(MdAtspiNode *n, const char *state) {
    n->states = realloc(n->states, sizeof(char*) * (n->state_count + 1));
    n->states[n->state_count++] = strdup(state);
}

static void node_add_child(MdAtspiNode *parent, MdAtspiNode *child) {
    parent->children = realloc(parent->children,
                                sizeof(MdAtspiNode*) * (parent->child_count + 1));
    parent->children[parent->child_count++] = child;
}

static void test_node_alloc_free(void) {
    md_atspi_node_free(NULL); /* should be safe */
    MdAtspiNode *n = make_node("1", "button", "OK", 0, 0, 80, 30);
    node_add_state(n, "enabled");
    md_atspi_node_free(n);
    printf("  PASS: node alloc/free\n");
}

static void test_create_destroy(void) {
    MdAtspiTree *tree = md_atspi_create();
    assert(tree != NULL);
    md_atspi_destroy(tree);
    printf("  PASS: create/destroy\n");
}

static void test_json_serialization(void) {
    /* Build: WIN[1] gedit -> BTN[42] Save, TXT[44] Hello */
    MdAtspiNode *root = make_node("node_1", "frame", "gedit - untitled",
                                   0, 0, 1920, 1080);
    node_add_state(root, "visible");
    node_add_state(root, "active");

    MdAtspiNode *btn = make_node("node_42", "push button", "Save", 10, 10, 80, 30);
    node_add_state(btn, "enabled");
    node_add_child(root, btn);

    MdAtspiNode *txt = make_node("node_44", "text", "Hello world", 10, 50, 400, 20);
    node_add_state(txt, "focused");
    node_add_child(root, txt);

    char *json = md_atspi_to_json(root);
    assert(json != NULL);
    assert(strstr(json, "\"v\":1") != NULL);
    assert(strstr(json, "\"id\":\"node_1\"") != NULL);
    assert(strstr(json, "\"role\":\"frame\"") != NULL);
    assert(strstr(json, "\"label\":\"gedit - untitled\"") != NULL);
    assert(strstr(json, "\"id\":\"node_42\"") != NULL);
    assert(strstr(json, "\"id\":\"node_44\"") != NULL);

    free(json);
    md_atspi_node_free(root);
    printf("  PASS: JSON serialization\n");
}

static void test_compact_serialization(void) {
    MdAtspiNode *root = make_node("1", "window", "gedit - untitled",
                                   0, 0, 1920, 1080);
    MdAtspiNode *btn = make_node("42", "button", "Save", 10, 10, 80, 30);
    node_add_state(btn, "enabled");
    node_add_child(root, btn);

    MdAtspiNode *txt = make_node("44", "entry", "Hello", 10, 50, 400, 20);
    node_add_state(txt, "focused");
    node_add_child(root, txt);

    char *compact = md_atspi_to_compact(root);
    assert(compact != NULL);
    assert(strstr(compact, "v1 ts:") != NULL);
    assert(strstr(compact, "WIN[1]") != NULL);
    assert(strstr(compact, "BTN[42] Save") != NULL);
    assert(strstr(compact, "*enabled*") != NULL);
    assert(strstr(compact, "TXT[44]") != NULL);
    assert(strstr(compact, "<focused>") != NULL);

    free(compact);
    md_atspi_node_free(root);
    printf("  PASS: compact serialization\n");
}

static void test_delta_serialization(void) {
    MdAtspiNode *node = make_node("node_5", "button", "New Button", 50, 50, 100, 30);
    MdAtspiDelta delta = {
        .op = MD_ATSPI_OP_ADD,
        .node = node,
        .parent_id = strdup("node_1"),
    };

    char *json = md_atspi_delta_to_json(&delta, 1);
    assert(json != NULL);
    assert(strstr(json, "\"op\":\"add\"") != NULL);
    assert(strstr(json, "\"id\":\"node_5\"") != NULL);
    assert(strstr(json, "\"parent_id\":\"node_1\"") != NULL);

    free(json);
    md_atspi_node_free(delta.node);
    free(delta.parent_id);
    printf("  PASS: delta serialization\n");
}

int main(void) {
    printf("test_atspi:\n");
    test_node_alloc_free();
    test_create_destroy();
    test_json_serialization();
    test_compact_serialization();
    test_delta_serialization();
    printf("All atspi tests passed.\n");
    return 0;
}
