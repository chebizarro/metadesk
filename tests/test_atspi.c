/*
 * metadesk — test_atspi.c
 * Accessibility tree HAL tests (serialisation + live backend if available).
 */
#include "a11y.h"
#include "a11y_query.h"
#ifdef __linux__
#include "atspi.h"
#include <atspi/atspi.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include "md_test.h"

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

static bool fixture_is_connected(const MdA11yCtx *ctx) {
    return ctx->backend_data && *(const bool *)ctx->backend_data;
}

static void test_liveness(void) {
    MD_CHECK(!md_a11y_is_connected(NULL));
    MdA11yBackend backend = { 0 };
    MdA11yCtx ctx = { .vtable = &backend };
    MD_CHECK(!md_a11y_is_connected(&ctx));
    bool connected = false;
    ctx.backend_data = &connected;
    MD_CHECK(md_a11y_is_connected(&ctx)); /* legacy fallback */
    backend.is_connected = fixture_is_connected;
    MD_CHECK(!md_a11y_is_connected(&ctx)); /* allocation is not liveness */
    connected = true;
    MD_CHECK(md_a11y_is_connected(&ctx));
    connected = false;
    MD_CHECK(!md_a11y_is_connected(&ctx));
    ctx.backend_data = NULL;
    MD_CHECK(!md_a11y_is_connected(&ctx));
    printf("  PASS: backend liveness callback and legacy fallback\n");
}

static void test_create_destroy(void) {
    if (!live_a11y_tests_enabled()) {
        printf("  SKIP: create/destroy live backend (set MD_A11Y_LIVE_TESTS=1)\n");
        return;
    }

    MdA11yCtx *ctx = md_a11y_create();
    MD_CHECK(ctx != NULL);
    MD_CHECK(md_a11y_is_connected(ctx));
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
    MD_CHECK(json != NULL);
    MD_CHECK(strstr(json, "\"v\":1") != NULL);
    MD_CHECK(strstr(json, "\"id\":\"node_1\"") != NULL);
    MD_CHECK(strstr(json, "\"role\":\"frame\"") != NULL);
    MD_CHECK(strstr(json, "\"label\":\"gedit - untitled\"") != NULL);
    MD_CHECK(strstr(json, "\"id\":\"node_42\"") != NULL);
    MD_CHECK(strstr(json, "\"id\":\"node_44\"") != NULL);

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
    MD_CHECK(compact != NULL);
    MD_CHECK(strstr(compact, "v1 ts:") != NULL);
    MD_CHECK(strstr(compact, "WIN[1]") != NULL);
    MD_CHECK(strstr(compact, "BTN[42] Save") != NULL);
    MD_CHECK(strstr(compact, "*enabled*") != NULL);
    MD_CHECK(strstr(compact, "TXT[44]") != NULL);
    MD_CHECK(strstr(compact, "<focused>") != NULL);

    /* §3.3.2: Text entries get quoted content with single quotes */
    MD_CHECK(strstr(compact, "'Hello'") != NULL);
    /* §3.3.2: <focused> appears before quoted content for text entries */
    {
        const char *focused = strstr(compact, "<focused>");
        const char *quoted = strstr(compact, "'Hello'");
        MD_CHECK(focused != NULL && quoted != NULL);
        MD_CHECK(focused < quoted);  /* <focused> before 'Hello' */
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
    MD_CHECK(compact != NULL);

    /* Window (container) present */
    MD_CHECK(strstr(compact, "WIN[1]") != NULL);
    /* Button under decorative panel still present */
    MD_CHECK(strstr(compact, "BTN[20] OK") != NULL);
    /* Decorative nodes skipped */
    MD_CHECK(strstr(compact, "PNL") == NULL);   /* panel filtered out */
    MD_CHECK(strstr(compact, "LBL") == NULL);   /* label filtered out */
    /* Dialog container present */
    MD_CHECK(strstr(compact, "DLG[40]") != NULL);
    /* Check box with state */
    MD_CHECK(strstr(compact, "CHK[50] Remember") != NULL);
    MD_CHECK(strstr(compact, "*checked*") != NULL);

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
    MD_CHECK(compact != NULL);

    /* Empty text gets quoted empty string */
    MD_CHECK(strstr(compact, "TXT[5] <focused> ''") != NULL);
    /* enabled state still appended after quoted content */
    MD_CHECK(strstr(compact, "*enabled*") != NULL);

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
    MD_CHECK(json != NULL);
    MD_CHECK(strstr(json, "\"op\":\"add\"") != NULL);
    MD_CHECK(strstr(json, "\"id\":\"node_5\"") != NULL);
    MD_CHECK(strstr(json, "\"parent_id\":\"node_1\"") != NULL);

    free(json);
    md_a11y_node_free(delta.node);
    free(delta.parent_id);
    printf("  PASS: delta serialization\n");
}

#ifdef __APPLE__
extern size_t md_a11y_axui_role_mapping_count(void);
extern enum MdA11yRole md_a11y_axui_role_mapping_at(size_t index);
extern const char *md_a11y_axui_native_role_at(size_t index);
extern const char *md_a11y_axui_normalize_role(const char *ax_role);
#endif

static void test_compact_table_filtering(void) {
    MdA11yNode *table = make_node("table", "table", "Data", 0, 0, 100, 100);
    for (int r = 0; r < 3; r++) {
        char id[16];
        snprintf(id, sizeof(id), "row%d", r);
        MdA11yNode *row = make_node(id, "table row", "Row", 0, 0, 100, 10);
        node_add_child(table, row);
        for (int c = 0; c < 2; c++) {
            snprintf(id, sizeof(id), "cell%d%d", r, c);
            node_add_child(row, make_node(id, "table cell", "Cell", 0, 0, 50, 10));
        }
    }
    char *compact = md_a11y_to_compact_at(table, 0);
    MD_CHECK(compact && strcmp(compact, "v1 ts:0\nTBL[table] Data\n") == 0);
    free(compact);
    md_a11y_node_free(table);

    static const enum MdA11yRole original_interactable[] = {
        MD_A11Y_ROLE_WINDOW, MD_A11Y_ROLE_APPLICATION, MD_A11Y_ROLE_DESKTOP,
        MD_A11Y_ROLE_DIALOG, MD_A11Y_ROLE_BUTTON, MD_A11Y_ROLE_TOGGLE_BUTTON,
        MD_A11Y_ROLE_TEXT, MD_A11Y_ROLE_MENU, MD_A11Y_ROLE_MENU_ITEM,
        MD_A11Y_ROLE_CHECK_BOX, MD_A11Y_ROLE_RADIO_BUTTON, MD_A11Y_ROLE_COMBO_BOX,
        MD_A11Y_ROLE_LIST, MD_A11Y_ROLE_LIST_ITEM, MD_A11Y_ROLE_TAB,
        MD_A11Y_ROLE_LINK, MD_A11Y_ROLE_SLIDER, MD_A11Y_ROLE_SPIN_BUTTON,
        MD_A11Y_ROLE_TREE, MD_A11Y_ROLE_TABLE,
    };
    for (int i = 0; i < MD_A11Y_ROLE_COUNT; i++) {
        bool expected = false;
        for (size_t j = 0; j < sizeof(original_interactable) / sizeof(original_interactable[0]); j++)
            if ((enum MdA11yRole)i == original_interactable[j]) expected = true;
        MD_CHECK(md_a11y_role_is_interactable_enum((enum MdA11yRole)i) == expected);
        MD_CHECK(md_a11y_role_is_interactable(md_a11y_role_name((enum MdA11yRole)i)) == expected);
    }
    printf("  PASS: compact 3x2 table golden and original interactable set\n");
}

static void test_vocab(void) {
    for (int i = 0; i < MD_A11Y_ROLE_COUNT; i++) {
        const char *name = md_a11y_role_name((enum MdA11yRole)i);
        MD_CHECK(md_a11y_role_lookup(name) == (enum MdA11yRole)i);
        for (int j = 0; j < i; j++) {
            const char *other = md_a11y_role_name((enum MdA11yRole)j);
            MD_CHECK(strcmp(name, other) != 0);
            MD_CHECK(strcmp(md_a11y_role_abbrev(name), md_a11y_role_abbrev(other)) != 0);
        }
    }
#define CHECK_ALIAS(name, role) \
    MD_CHECK(role >= 0 && role < MD_A11Y_ROLE_COUNT); \
    MD_CHECK(md_a11y_role_lookup(name) == role);
    MD_A11Y_ROLE_ALIASES(CHECK_ALIAS)
#undef CHECK_ALIAS
    for (int i = 0; i < MD_A11Y_STATE_COUNT; i++) {
        const char *name = md_a11y_state_name((enum MdA11yState)i);
        MD_CHECK(name != NULL);
        MD_CHECK(md_a11y_state_lookup(name) == (enum MdA11yState)i);
    }
    MD_CHECK(md_a11y_role_lookup(NULL) == MD_A11Y_ROLE_UNKNOWN);
    MD_CHECK(md_a11y_state_lookup("not-a-state") == MD_A11Y_STATE_UNKNOWN);
#ifdef __APPLE__
    MD_CHECK(md_a11y_axui_role_mapping_count() > 0);
    for (size_t i = 0; i < md_a11y_axui_role_mapping_count(); i++) {
        enum MdA11yRole role = md_a11y_axui_role_mapping_at(i);
        MD_CHECK(role >= MD_A11Y_ROLE_UNKNOWN && role < MD_A11Y_ROLE_COUNT);
        MD_CHECK(md_a11y_role_lookup(md_a11y_role_name(role)) == role);
        const char *native = md_a11y_axui_native_role_at(i);
        MD_CHECK(native != NULL);
        MD_CHECK(strcmp(md_a11y_axui_normalize_role(native), md_a11y_role_name(role)) == 0);
    }
    static const struct { const char *native; enum MdA11yRole role; } added_roles[] = {
        { "AXMenuButton", MD_A11Y_ROLE_BUTTON },
        { "AXColorWell", MD_A11Y_ROLE_COLOR_CHOOSER },
        { "AXDateField", MD_A11Y_ROLE_DATE_EDITOR },
        { "AXTimeField", MD_A11Y_ROLE_DATE_EDITOR },
        { "AXIncrementor", MD_A11Y_ROLE_SPIN_BUTTON },
        { "AXLevelIndicator", MD_A11Y_ROLE_PROGRESS_BAR },
        { "AXBusyIndicator", MD_A11Y_ROLE_PROGRESS_BAR },
        { "AXGrid", MD_A11Y_ROLE_TABLE },
        { "AXDockItem", MD_A11Y_ROLE_BUTTON },
        { "AXRadioGroup", MD_A11Y_ROLE_PANEL },
        { "AXPopover", MD_A11Y_ROLE_DIALOG },
        { "AXListMarker", MD_A11Y_ROLE_LABEL },
        { "AXOpaqueProviderGroup", MD_A11Y_ROLE_PANEL },
        { "AXRuler", MD_A11Y_ROLE_UNKNOWN },
        { "AXRulerMarker", MD_A11Y_ROLE_UNKNOWN },
        { "AXUnknown", MD_A11Y_ROLE_UNKNOWN },
        { "AXSystemWide", MD_A11Y_ROLE_UNKNOWN },
    };
    for (size_t i = 0; i < sizeof(added_roles) / sizeof(added_roles[0]); i++)
        MD_CHECK(strcmp(md_a11y_axui_normalize_role(added_roles[i].native),
                        md_a11y_role_name(added_roles[i].role)) == 0);
    for (int i = 0; i < 2; i++)
        MD_CHECK(strcmp(md_a11y_axui_normalize_role("AXUnmappedReviewFixture"), "unknown") == 0);
#endif
    MD_CHECK(md_a11y_role_lookup("header") == MD_A11Y_ROLE_PANEL);
    MD_CHECK(md_a11y_role_lookup("footer") == MD_A11Y_ROLE_PANEL);
    MdA11yNode custom = { .id = "custom", .role = "vendor widget", .label = "Custom" };
    char *raw_states[] = { "vendor state", "focused", NULL };
    custom.states = raw_states;
    custom.state_count = 3;
    MD_CHECK(md_a11y_matches(&custom, "VENDOR WIDGET", NULL, "VENDOR STATE"));
    MD_CHECK(md_a11y_matches(&custom, NULL, NULL, "FOCUSED"));
    MD_CHECK(!md_a11y_matches(&custom, "button", NULL, NULL));
    MD_CHECK(!md_a11y_matches(&custom, NULL, NULL, "enabled"));
    custom.states = NULL;
    custom.state_count = 0;
    char *targeted = md_a11y_query_to_compact(&custom, 0);
    MD_CHECK(targeted && strcmp(targeted, "vendor widget[custom] Custom\n") == 0);
    free(targeted);
    char *full = md_a11y_to_compact_at(&custom, 0);
    MD_CHECK(full && strcmp(full, "v1 ts:0\n") == 0);
    free(full);
    printf("  PASS: shared vocabulary and native role mappings\n");
}

static void check_live_vocab(const MdA11yNode *node, int *unknown_roles) {
    enum MdA11yRole role = md_a11y_role_lookup(node->role);
    if (role == MD_A11Y_ROLE_UNKNOWN) {
        printf("  live unknown role: %s (id=%s)\n", node->role ? node->role : "(null)",
               node->id ? node->id : "?");
        (*unknown_roles)++;
    }
    /* UNKNOWN is a documented canonical sentinel, not an ad-hoc native string. */
    MD_CHECK(node->role && strcmp(node->role, md_a11y_role_name(role)) == 0);
    for (int i = 0; i < node->state_count; i++) {
        enum MdA11yState state = md_a11y_state_lookup(node->states[i]);
        if (state == MD_A11Y_STATE_UNKNOWN)
            printf("  live unknown state: %s\n", node->states[i] ? node->states[i] : "(null)");
        MD_CHECK(state != MD_A11Y_STATE_UNKNOWN);
    }
    for (int i = 0; i < node->child_count; i++)
        check_live_vocab(node->children[i], unknown_roles);
}

static void test_walk_tree(void) {
    if (!live_a11y_tests_enabled()) {
        printf("  SKIP: walk tree live backend (set MD_A11Y_LIVE_TESTS=1)\n");
        return;
    }

    MdA11yCtx *ctx = md_a11y_create();
    MD_CHECK(ctx != NULL);

    MdA11yNode *root = md_a11y_walk(ctx);
    if (!root) {
        /* Accessibility bus may not be available in CI/headless environments */
        printf("  SKIP: walk tree (no accessibility bus)\n");
        md_a11y_destroy(ctx);
        return;
    }

    /* Basic sanity: root should exist with role and id */
    MD_CHECK(root->id != NULL);
    MD_CHECK(root->role != NULL);
    MD_CHECK(strcmp(root->role, "desktop") == 0);

    int unknown_roles = 0;
    check_live_vocab(root, &unknown_roles);
    printf("  live vocabulary: %d unknown-role nodes\n", unknown_roles);

    /* Serialize in both formats to verify end-to-end */
    char *json = md_a11y_to_json(root);
    MD_CHECK(json != NULL);
    MD_CHECK(strstr(json, "\"role\":\"desktop\"") != NULL);
    free(json);

    char *compact = md_a11y_to_compact(root);
    MD_CHECK(compact != NULL);
    MD_CHECK(strstr(compact, "DSK[") != NULL);
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
    MD_CHECK(ctx != NULL);

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

static int (*live_get_tree)(MdA11yCtx *, MdA11yNode **);
static atomic_int live_walks;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int hints;
} LiveHints;

static int counted_live_get_tree(MdA11yCtx *ctx, MdA11yNode **out) {
    atomic_fetch_add(&live_walks, 1);
    return live_get_tree(ctx, out);
}

static void live_change_cb(const MdA11yDelta *deltas, int count, void *userdata) {
    LiveHints *h = userdata;
    MD_CHECK(deltas == NULL && count == 0);
    pthread_mutex_lock(&h->mu);
    h->hints++;
    pthread_cond_signal(&h->cv);
    pthread_mutex_unlock(&h->mu);
}

static void test_subscribe_changes(void) {
    if (!live_a11y_tests_enabled()) {
        printf("  SKIP: subscribe live backend (set MD_A11Y_LIVE_TESTS=1)\n");
        return;
    }

    MdA11yCtx *ctx = md_a11y_create();
    MD_CHECK(ctx != NULL);

    MdA11yBackend counted = *ctx->vtable;
    live_get_tree = counted.get_tree;
    atomic_store(&live_walks, 0);
    counted.get_tree = counted_live_get_tree;
    ctx->vtable = &counted;
    LiveHints hints = { .mu = PTHREAD_MUTEX_INITIALIZER,
                        .cv = PTHREAD_COND_INITIALIZER };
    int rc = md_a11y_subscribe_changes(ctx, live_change_cb, &hints);
    if (rc < 0) {
        /* Accessibility bus may not be available in CI/headless environments.
         * Windows UIA subscriptions also require an interactive desktop. */
        printf("  SKIP: subscribe changes (no accessibility event bus)\n");
        pthread_cond_destroy(&hints.cv);
        pthread_mutex_destroy(&hints.mu);
        md_a11y_destroy(ctx);
        return;
    }

    MD_CHECK(rc >= 0);
    int subscribe_walks = atomic_load(&live_walks);
    MD_CHECK(subscribe_walks == 0); /* subscribing must not walk or seed agent diff state */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    pthread_mutex_lock(&hints.mu);
    while (!hints.hints) {
        if (pthread_cond_timedwait(&hints.cv, &hints.mu, &deadline) != 0) break;
    }
    pthread_mutex_unlock(&hints.mu);
    MD_CHECK(md_a11y_unsubscribe_changes(ctx) == 0);
    MD_CHECK(atomic_load(&live_walks) == subscribe_walks);
    if (hints.hints)
        printf("  PASS: %d live change hints caused zero get_tree calls\n", hints.hints);
    else
        printf("  SKIP: live hint walk check (no OS event during observation)\n");
    pthread_cond_destroy(&hints.cv);
    pthread_mutex_destroy(&hints.mu);
    md_a11y_destroy(ctx);
    printf("  PASS: subscribe changes\n");
}

/* ── Tree patch tests (§3.3.3 in-place delta application) ──── */

static void test_tree_patch_add(void) {
    /* Build a tree, serialize, then add a node via delta */
    MdA11yNode *root = make_node("r1", "window", "App", 0, 0, 800, 600);
    MdA11yNode *btn = make_node("b1", "button", "OK", 10, 10, 80, 30);
    node_add_child(root, btn);

    char *tree = md_a11y_to_json(root);
    MD_CHECK(tree != NULL);

    /* Delta: add a new button under root */
    const char *delta = "[{\"op\":\"add\",\"parent_id\":\"r1\","
        "\"node\":{\"id\":\"b2\",\"role\":\"button\",\"label\":\"Cancel\","
        "\"bounds\":{\"x\":100,\"y\":10,\"w\":80,\"h\":30},\"children\":[]}}]";

    char *patched = md_a11y_tree_patch(tree, delta);
    MD_CHECK(patched != NULL);

    /* Verify the new node appears */
    MD_CHECK(strstr(patched, "\"b2\"") != NULL);
    MD_CHECK(strstr(patched, "Cancel") != NULL);
    /* Original node still present */
    MD_CHECK(strstr(patched, "\"b1\"") != NULL);

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
    MD_CHECK(tree != NULL);

    /* Delta: remove b1 */
    const char *delta = "[{\"op\":\"remove\",\"node\":{\"id\":\"b1\"}}]";

    char *patched = md_a11y_tree_patch(tree, delta);
    MD_CHECK(patched != NULL);

    /* b1 should be gone, b2 still present */
    MD_CHECK(strstr(patched, "\"b1\"") == NULL);
    MD_CHECK(strstr(patched, "\"b2\"") != NULL);

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
    MD_CHECK(tree != NULL);

    /* Delta: update b1's label */
    const char *delta = "[{\"op\":\"update\","
        "\"node\":{\"id\":\"b1\",\"label\":\"Confirm\"}}]";

    char *patched = md_a11y_tree_patch(tree, delta);
    MD_CHECK(patched != NULL);

    /* New label should appear, old should not */
    MD_CHECK(strstr(patched, "Confirm") != NULL);
    /* node id still present */
    MD_CHECK(strstr(patched, "\"b1\"") != NULL);

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
    MD_CHECK(tree != NULL);

    /* Multiple ops: remove b1, update b2, add b3 */
    const char *delta =
        "[{\"op\":\"remove\",\"node\":{\"id\":\"b1\"}},"
        "{\"op\":\"update\",\"node\":{\"id\":\"b2\",\"label\":\"Updated\"}},"
        "{\"op\":\"add\",\"parent_id\":\"r1\","
        "\"node\":{\"id\":\"b3\",\"role\":\"button\",\"label\":\"New\","
        "\"bounds\":{\"x\":0,\"y\":0,\"w\":80,\"h\":30},\"children\":[]}}]";

    char *patched = md_a11y_tree_patch(tree, delta);
    MD_CHECK(patched != NULL);

    MD_CHECK(strstr(patched, "\"b1\"") == NULL);     /* removed */
    MD_CHECK(strstr(patched, "Updated") != NULL);     /* updated */
    MD_CHECK(strstr(patched, "\"b3\"") != NULL);       /* added */

    free(patched);
    free(tree);
    md_a11y_node_free(root);
    printf("  PASS: tree patch multiple ops\n");
}

static void test_tree_patch_null_safety(void) {
    MD_CHECK(md_a11y_tree_patch(NULL, "[]") == NULL);
    MD_CHECK(md_a11y_tree_patch("{}", NULL) == NULL);
    MD_CHECK(md_a11y_tree_patch("not json", "[]") == NULL);
    MD_CHECK(md_a11y_tree_patch("{\"v\":1}", "not json") == NULL);

    printf("  PASS: tree patch null safety\n");
}

static void test_tree_patch_deep_add(void) {
    /* Add a node under a nested child, not the root */
    MdA11yNode *root = make_node("r1", "window", "App", 0, 0, 800, 600);
    MdA11yNode *panel = make_node("p1", "panel", "Menu", 0, 0, 200, 600);
    node_add_child(root, panel);

    char *tree = md_a11y_to_json(root);
    MD_CHECK(tree != NULL);

    /* Add button under panel (p1), not root */
    const char *delta = "[{\"op\":\"add\",\"parent_id\":\"p1\","
        "\"node\":{\"id\":\"m1\",\"role\":\"menu item\",\"label\":\"File\","
        "\"bounds\":{\"x\":5,\"y\":5,\"w\":100,\"h\":20},\"children\":[]}}]";

    char *patched = md_a11y_tree_patch(tree, delta);
    MD_CHECK(patched != NULL);

    MD_CHECK(strstr(patched, "\"m1\"") != NULL);
    MD_CHECK(strstr(patched, "File") != NULL);

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
    MD_CHECK(tree != NULL);

    /* Update b1's state to focused */
    const char *delta = "[{\"op\":\"update\","
        "\"node\":{\"id\":\"b1\",\"state\":[\"focused\",\"pressed\"]}}]";

    char *patched = md_a11y_tree_patch(tree, delta);
    MD_CHECK(patched != NULL);

    MD_CHECK(strstr(patched, "focused") != NULL);
    MD_CHECK(strstr(patched, "pressed") != NULL);

    free(patched);
    free(tree);
    md_a11y_node_free(root);
    printf("  PASS: tree patch update state\n");
}

#ifdef __linux__
/* md-03d: the AT-SPI backend used to size node->states to the full
 * AtspiStateSet length but only fill the mapped entries, leaving NULL holes
 * for states metadesk has no vocabulary for (HORIZONTAL, OPAQUE, ...).
 * Exercise the mapping with a synthetic state set — no a11y bus needed. */
static void test_atspi_state_compaction(void) {
    AtspiStateType raw[] = {
        ATSPI_STATE_ACTIVE,
        ATSPI_STATE_HORIZONTAL,          /* unmapped */
        ATSPI_STATE_FOCUSED,
        ATSPI_STATE_MANAGES_DESCENDANTS, /* unmapped */
        ATSPI_STATE_OPAQUE,              /* unmapped */
        ATSPI_STATE_ENABLED,
    };
    GArray *arr = g_array_sized_new(FALSE, FALSE, sizeof(AtspiStateType),
                                    (guint)(sizeof(raw) / sizeof(raw[0])));
    g_array_append_vals(arr, raw, (guint)(sizeof(raw) / sizeof(raw[0])));
    AtspiStateSet *set = atspi_state_set_new(arr);
    g_array_free(arr, TRUE);
    MD_CHECK(set != NULL);

    MdA11yNode *node = calloc(1, sizeof(*node));
    MD_CHECK(node != NULL);
    md_atspi_extract_states(set, node);

    MD_CHECK(node->state_count == 3);
    MD_CHECK(node->states != NULL);
    for (int i = 0; i < node->state_count; i++)
        MD_CHECK(node->states[i] != NULL);
    /* AtspiStateSet is a bitset: get_states() yields enum order, not
     * insertion order, so check membership rather than position. */
    bool saw_active = false, saw_focused = false, saw_enabled = false;
    for (int i = 0; node->states && i < node->state_count; i++) {
        if (!node->states[i]) continue;
        if (strcmp(node->states[i], "active") == 0)  saw_active = true;
        if (strcmp(node->states[i], "focused") == 0) saw_focused = true;
        if (strcmp(node->states[i], "enabled") == 0) saw_enabled = true;
    }
    MD_CHECK(saw_active && saw_focused && saw_enabled);

    /* All-unmapped set: no array, count stays zero. */
    MdA11yNode *empty = calloc(1, sizeof(*empty));
    MD_CHECK(empty != NULL);
    AtspiStateType none[] = { ATSPI_STATE_HORIZONTAL, ATSPI_STATE_OPAQUE };
    GArray *arr2 = g_array_sized_new(FALSE, FALSE, sizeof(AtspiStateType), 2);
    g_array_append_vals(arr2, none, 2);
    AtspiStateSet *set2 = atspi_state_set_new(arr2);
    g_array_free(arr2, TRUE);
    md_atspi_extract_states(set2, empty);
    MD_CHECK(empty->state_count == 0 && empty->states == NULL);

    md_a11y_node_free(node);
    md_a11y_node_free(empty);
    g_object_unref(set);
    g_object_unref(set2);
    PASS("AT-SPI state extraction compacts unmapped states (no NULL holes)");
}
#endif

int main(void) {
    printf("test_a11y:\n");
    printf("  live backend tests: %s\n", live_a11y_tests_enabled() ? "enabled" : "disabled");
    test_liveness();
    test_vocab();
#ifdef __linux__
    test_atspi_state_compaction();
#endif
    test_compact_table_filtering();
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
    test_subscribe_changes();
    printf("All a11y tests passed.\n");
    return md_test_report();
}
