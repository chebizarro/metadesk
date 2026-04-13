/*
 * metadesk — a11y_axui.m
 * macOS accessibility backend: AXUIElement (spec §2.3.2).
 *
 * Uses the macOS Accessibility API to:
 *   1. Check AXIsProcessTrusted() for permissions
 *   2. Walk the AX hierarchy from the system-wide element
 *   3. Extract: AXRole, AXTitle/AXDescription, AXFrame, AXEnabled etc.
 *   4. Build an MdA11yNode tree
 *   5. Compute deltas by comparing current vs previous snapshot
 *   6. (Phase 2) AXObserver for live change notifications
 *
 * Serialization is handled by the platform-agnostic a11y.c.
 *
 * Requires user to grant accessibility in System Settings >
 * Privacy & Security > Accessibility.
 *
 * Built as Objective-C (.m) for NSWorkspace, NSRunningApplication.
 */
#include "a11y.h"

#import <ApplicationServices/ApplicationServices.h>
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Maximum tree depth to prevent infinite recursion */
#define MD_AX_MAX_DEPTH 32

/* Maximum children per node */
#define MD_AX_MAX_CHILDREN 256

/* ── Backend-private state ───────────────────────────────────── */

typedef struct {
    int          connected;
    uint64_t     next_id;
    MdA11yNode  *last_snapshot;
} AXUIState;

/* ── Helpers ─────────────────────────────────────────────────── */

static char *make_node_id(AXUIState *st) {
    char buf[32];
    snprintf(buf, sizeof(buf), "n%lu", (unsigned long)st->next_id++);
    return strdup(buf);
}

/* Get a string attribute from an AXUIElement, or NULL. Caller frees. */
static char *ax_get_string(AXUIElementRef elem, CFStringRef attr) {
    CFTypeRef value = NULL;
    AXError err = AXUIElementCopyAttributeValue(elem, attr, &value);
    if (err != kAXErrorSuccess || !value)
        return NULL;

    if (CFGetTypeID(value) != CFStringGetTypeID()) {
        CFRelease(value);
        return NULL;
    }

    CFStringRef str = (CFStringRef)value;
    CFIndex len = CFStringGetMaximumSizeForEncoding(
        CFStringGetLength(str), kCFStringEncodingUTF8) + 1;
    char *buf = malloc((size_t)len);
    if (!buf) { CFRelease(value); return NULL; }

    if (!CFStringGetCString(str, buf, len, kCFStringEncodingUTF8)) {
        free(buf);
        CFRelease(value);
        return NULL;
    }

    CFRelease(value);
    return buf;
}

/* Get a boolean attribute, returns -1 on error, 0 or 1 otherwise. */
static int ax_get_bool(AXUIElementRef elem, CFStringRef attr) {
    CFTypeRef value = NULL;
    AXError err = AXUIElementCopyAttributeValue(elem, attr, &value);
    if (err != kAXErrorSuccess || !value)
        return -1;

    if (CFGetTypeID(value) != CFBooleanGetTypeID()) {
        CFRelease(value);
        return -1;
    }

    int result = CFBooleanGetValue((CFBooleanRef)value) ? 1 : 0;
    CFRelease(value);
    return result;
}

/* Normalize AX role strings to platform-neutral names matching AT-SPI2 */
static const char *normalize_role(const char *ax_role) {
    if (!ax_role) return "unknown";

    /* Map common AX roles to platform-neutral names */
    if (strcmp(ax_role, "AXApplication") == 0)  return "application";
    if (strcmp(ax_role, "AXWindow") == 0)       return "window";
    if (strcmp(ax_role, "AXButton") == 0)       return "push button";
    if (strcmp(ax_role, "AXCheckBox") == 0)     return "check box";
    if (strcmp(ax_role, "AXRadioButton") == 0)  return "radio button";
    if (strcmp(ax_role, "AXTextField") == 0)    return "text";
    if (strcmp(ax_role, "AXTextArea") == 0)     return "text";
    if (strcmp(ax_role, "AXStaticText") == 0)   return "label";
    if (strcmp(ax_role, "AXImage") == 0)        return "image";
    if (strcmp(ax_role, "AXGroup") == 0)        return "panel";
    if (strcmp(ax_role, "AXList") == 0)         return "list";
    if (strcmp(ax_role, "AXTable") == 0)        return "table";
    if (strcmp(ax_role, "AXRow") == 0)          return "table row";
    if (strcmp(ax_role, "AXColumn") == 0)       return "table column";
    if (strcmp(ax_role, "AXCell") == 0)         return "table cell";
    if (strcmp(ax_role, "AXScrollArea") == 0)   return "scroll pane";
    if (strcmp(ax_role, "AXScrollBar") == 0)    return "scroll bar";
    if (strcmp(ax_role, "AXSlider") == 0)       return "slider";
    if (strcmp(ax_role, "AXMenuBar") == 0)      return "menu bar";
    if (strcmp(ax_role, "AXMenu") == 0)         return "menu";
    if (strcmp(ax_role, "AXMenuItem") == 0)     return "menu item";
    if (strcmp(ax_role, "AXToolbar") == 0)      return "tool bar";
    if (strcmp(ax_role, "AXTabGroup") == 0)     return "page tab list";
    if (strcmp(ax_role, "AXTab") == 0)          return "page tab";
    if (strcmp(ax_role, "AXComboBox") == 0)     return "combo box";
    if (strcmp(ax_role, "AXPopUpButton") == 0)  return "combo box";
    if (strcmp(ax_role, "AXProgressIndicator") == 0) return "progress bar";
    if (strcmp(ax_role, "AXLink") == 0)         return "link";
    if (strcmp(ax_role, "AXWebArea") == 0)      return "document web";
    if (strcmp(ax_role, "AXSheet") == 0)        return "dialog";
    if (strcmp(ax_role, "AXDialog") == 0)       return "dialog";
    if (strcmp(ax_role, "AXSplitGroup") == 0)   return "split pane";
    if (strcmp(ax_role, "AXOutline") == 0)      return "tree";
    if (strcmp(ax_role, "AXOutlineRow") == 0)   return "tree item";
    if (strcmp(ax_role, "AXHeading") == 0)      return "heading";

    /* Strip "AX" prefix for anything else */
    if (strncmp(ax_role, "AX", 2) == 0 && ax_role[2] != '\0')
        return ax_role + 2;

    return ax_role;
}

/* Extract states from AX attributes → state string array */
static void extract_states(AXUIElementRef elem, MdA11yNode *node) {
    /* Collect up to 8 states */
    char *states[8];
    int count = 0;

    int val;

    val = ax_get_bool(elem, kAXEnabledAttribute);
    if (val == 1) states[count++] = strdup("enabled");

    val = ax_get_bool(elem, kAXFocusedAttribute);
    if (val == 1) states[count++] = strdup("focused");

    /* Check if element is the frontmost/selected */
    val = ax_get_bool(elem, kAXSelectedAttribute);
    if (val == 1) states[count++] = strdup("selected");

    /* Check for expanded state (disclosure triangles, menus, etc.) */
    CFTypeRef expandedValue = NULL;
    if (AXUIElementCopyAttributeValue(elem, CFSTR("AXExpanded"), &expandedValue)
        == kAXErrorSuccess && expandedValue) {
        if (CFGetTypeID(expandedValue) == CFBooleanGetTypeID()) {
            states[count++] = strdup("expandable");
            if (CFBooleanGetValue((CFBooleanRef)expandedValue))
                states[count++] = strdup("expanded");
        }
        CFRelease(expandedValue);
    }

    /* Visibility: if element has a position and size, it's showing */
    CFTypeRef posValue = NULL;
    if (AXUIElementCopyAttributeValue(elem, kAXPositionAttribute, &posValue)
        == kAXErrorSuccess && posValue) {
        states[count++] = strdup("visible");
        states[count++] = strdup("showing");
        CFRelease(posValue);
    }

    if (count > 0) {
        node->states = calloc((size_t)count, sizeof(char *));
        if (node->states) {
            memcpy(node->states, states, (size_t)count * sizeof(char *));
            node->state_count = count;
        } else {
            for (int i = 0; i < count; i++) free(states[i]);
        }
    }
}

/* Extract bounding box from AXPosition + AXSize */
static void extract_bounds(AXUIElementRef elem, MdA11yNode *node) {
    CFTypeRef posValue = NULL, sizeValue = NULL;

    AXError err = AXUIElementCopyAttributeValue(elem, kAXPositionAttribute, &posValue);
    if (err != kAXErrorSuccess || !posValue) return;

    err = AXUIElementCopyAttributeValue(elem, kAXSizeAttribute, &sizeValue);
    if (err != kAXErrorSuccess || !sizeValue) {
        CFRelease(posValue);
        return;
    }

    CGPoint pos;
    CGSize size;
    if (AXValueGetValue(posValue, kAXValueCGPointType, &pos) &&
        AXValueGetValue(sizeValue, kAXValueCGSizeType, &size)) {
        node->x = (int)pos.x;
        node->y = (int)pos.y;
        node->w = (int)size.width;
        node->h = (int)size.height;
    }

    CFRelease(posValue);
    CFRelease(sizeValue);
}

/* ── Tree walking ────────────────────────────────────────────── */

static MdA11yNode *walk_element(AXUIState *st, AXUIElementRef elem, int depth) {
    if (!elem || depth > MD_AX_MAX_DEPTH)
        return NULL;

    MdA11yNode *node = calloc(1, sizeof(MdA11yNode));
    if (!node) return NULL;

    node->id = make_node_id(st);

    /* Role */
    char *raw_role = ax_get_string(elem, kAXRoleAttribute);
    const char *normalized = normalize_role(raw_role);
    node->role = strdup(normalized);
    free(raw_role);

    /* Label: try AXTitle first, fall back to AXDescription */
    node->label = ax_get_string(elem, kAXTitleAttribute);
    if (!node->label || node->label[0] == '\0') {
        free(node->label);
        node->label = ax_get_string(elem, kAXDescriptionAttribute);
    }
    /* Last resort: AXValue for text fields */
    if (!node->label || node->label[0] == '\0') {
        char *role_str = ax_get_string(elem, kAXRoleAttribute);
        if (role_str && (strcmp(role_str, "AXStaticText") == 0 ||
                         strcmp(role_str, "AXTextField") == 0)) {
            free(node->label);
            node->label = ax_get_string(elem, kAXValueAttribute);
        }
        free(role_str);
    }

    /* States */
    extract_states(elem, node);

    /* Bounds */
    extract_bounds(elem, node);

    /* Children */
    CFTypeRef childrenRef = NULL;
    AXError err = AXUIElementCopyAttributeValue(elem, kAXChildrenAttribute,
                                                 &childrenRef);
    if (err == kAXErrorSuccess && childrenRef &&
        CFGetTypeID(childrenRef) == CFArrayGetTypeID()) {
        CFArrayRef children = (CFArrayRef)childrenRef;
        CFIndex count = CFArrayGetCount(children);
        if (count > MD_AX_MAX_CHILDREN)
            count = MD_AX_MAX_CHILDREN;

        if (count > 0) {
            node->children = calloc((size_t)count, sizeof(MdA11yNode *));
            if (node->children) {
                int actual = 0;
                for (CFIndex i = 0; i < count; i++) {
                    AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);
                    MdA11yNode *child_node = walk_element(st, child, depth + 1);
                    if (child_node)
                        node->children[actual++] = child_node;
                }
                node->child_count = actual;
            }
        }
    }
    if (childrenRef) CFRelease(childrenRef);

    return node;
}

/* ── Delta computation (same algorithm as a11y_atspi.c) ──────── */

typedef struct {
    const char        *id;
    const MdA11yNode  *node;
} FlatEntry;

static void flatten_tree(const MdA11yNode *node, FlatEntry **entries,
                         int *count, int *capacity) {
    if (!node) return;

    if (*count >= *capacity) {
        *capacity = (*capacity == 0) ? 64 : *capacity * 2;
        *entries = realloc(*entries, (size_t)*capacity * sizeof(FlatEntry));
        if (!*entries) { *count = 0; return; }
    }

    (*entries)[*count].id = node->id;
    (*entries)[*count].node = node;
    (*count)++;

    for (int i = 0; i < node->child_count && node->children; i++)
        flatten_tree(node->children[i], entries, count, capacity);
}

static const FlatEntry *find_by_id(const FlatEntry *entries, int count,
                                   const char *id) {
    if (!id) return NULL;
    for (int i = 0; i < count; i++) {
        if (entries[i].id && strcmp(entries[i].id, id) == 0)
            return &entries[i];
    }
    return NULL;
}

static bool nodes_differ(const MdA11yNode *a, const MdA11yNode *b) {
    if (!a || !b) return true;
    if ((a->role == NULL) != (b->role == NULL)) return true;
    if (a->role && b->role && strcmp(a->role, b->role) != 0) return true;
    if ((a->label == NULL) != (b->label == NULL)) return true;
    if (a->label && b->label && strcmp(a->label, b->label) != 0) return true;
    if (a->x != b->x || a->y != b->y || a->w != b->w || a->h != b->h)
        return true;
    if (a->state_count != b->state_count) return true;
    return false;
}

static MdA11yNode *clone_node_shallow(const MdA11yNode *src) {
    if (!src) return NULL;

    MdA11yNode *dst = calloc(1, sizeof(MdA11yNode));
    if (!dst) return NULL;

    if (src->id)    dst->id    = strdup(src->id);
    if (src->role)  dst->role  = strdup(src->role);
    if (src->label) dst->label = strdup(src->label);
    dst->x = src->x;
    dst->y = src->y;
    dst->w = src->w;
    dst->h = src->h;

    if (src->state_count > 0 && src->states) {
        dst->states = calloc((size_t)src->state_count, sizeof(char *));
        if (dst->states) {
            dst->state_count = src->state_count;
            for (int i = 0; i < src->state_count; i++) {
                if (src->states[i])
                    dst->states[i] = strdup(src->states[i]);
            }
        }
    }

    return dst;
}

/* ── Vtable implementation ───────────────────────────────────── */

static int axui_init(MdA11yCtx *ctx) {
    /* Check accessibility permission */
    NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES};
    Boolean trusted = AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
    if (!trusted) {
        fprintf(stderr, "a11y_axui: accessibility permission not granted.\n"
                "Please enable in System Settings > Privacy & Security > Accessibility.\n");
        /* Don't fail — permission may be granted while we're running.
         * get_tree will return empty results until granted. */
    }

    AXUIState *st = calloc(1, sizeof(AXUIState));
    if (!st) return -1;

    st->connected = 1;
    ctx->backend_data = st;
    return 0;
}

static int axui_get_tree(MdA11yCtx *ctx, MdA11yNode **out_root) {
    AXUIState *st = ctx->backend_data;
    if (!st || !st->connected || !out_root) return -1;

    st->next_id = 0;

    /* Create root "desktop" node */
    MdA11yNode *root = calloc(1, sizeof(MdA11yNode));
    if (!root) return -1;

    root->id = make_node_id(st);
    root->role = strdup("desktop");
    root->label = strdup("Desktop");

    /* Get all running applications */
    @autoreleasepool {
        NSArray<NSRunningApplication *> *apps =
            [[NSWorkspace sharedWorkspace] runningApplications];

        /* Count GUI apps (those with an activation policy of regular or accessory) */
        NSMutableArray<NSRunningApplication *> *guiApps = [NSMutableArray array];
        for (NSRunningApplication *app in apps) {
            if (app.activationPolicy == NSApplicationActivationPolicyRegular ||
                app.activationPolicy == NSApplicationActivationPolicyAccessory) {
                [guiApps addObject:app];
            }
        }

        int app_count = (int)guiApps.count;
        if (app_count > MD_AX_MAX_CHILDREN)
            app_count = MD_AX_MAX_CHILDREN;

        if (app_count > 0) {
            root->children = calloc((size_t)app_count, sizeof(MdA11yNode *));
            if (root->children) {
                int actual = 0;
                for (int i = 0; i < app_count; i++) {
                    NSRunningApplication *nsApp = guiApps[(NSUInteger)i];
                    pid_t pid = nsApp.processIdentifier;

                    AXUIElementRef appElem = AXUIElementCreateApplication(pid);
                    if (!appElem) continue;

                    MdA11yNode *app_node = walk_element(st, appElem, 1);
                    if (app_node) {
                        /* Override label with localized app name if available */
                        if (nsApp.localizedName) {
                            free(app_node->label);
                            app_node->label = strdup([nsApp.localizedName UTF8String]);
                        }
                        root->children[actual++] = app_node;
                    }

                    CFRelease(appElem);
                }
                root->child_count = actual;
            }
        }
    }

    *out_root = root;
    return 0;
}

static int axui_get_diff(MdA11yCtx *ctx, MdA11yDelta **out_deltas,
                         int *out_count) {
    AXUIState *st = ctx->backend_data;
    if (!st || !out_deltas || !out_count) return -1;

    *out_deltas = NULL;
    *out_count = 0;

    MdA11yNode *current = NULL;
    if (axui_get_tree(ctx, &current) != 0 || !current)
        return -1;

    MdA11yNode *prev = st->last_snapshot;

    /* No previous snapshot — everything is new */
    if (!prev) {
        st->last_snapshot = current;
        return 0;
    }

    /* Flatten both trees */
    FlatEntry *prev_flat = NULL, *curr_flat = NULL;
    int prev_count = 0, curr_count = 0;
    int prev_cap = 0, curr_cap = 0;

    flatten_tree(prev, &prev_flat, &prev_count, &prev_cap);
    flatten_tree(current, &curr_flat, &curr_count, &curr_cap);

    int max_deltas = prev_count + curr_count;
    MdA11yDelta *deltas = calloc((size_t)max_deltas, sizeof(MdA11yDelta));
    if (!deltas) {
        free(prev_flat);
        free(curr_flat);
        md_a11y_node_free(current);
        return -1;
    }

    int dc = 0;

    /* Removed nodes */
    for (int i = 0; i < prev_count; i++) {
        if (!find_by_id(curr_flat, curr_count, prev_flat[i].id)) {
            deltas[dc].op = MD_A11Y_OP_REMOVE;
            deltas[dc].node = clone_node_shallow(prev_flat[i].node);
            dc++;
        }
    }

    /* Added and updated nodes */
    for (int i = 0; i < curr_count; i++) {
        const FlatEntry *prev_entry = find_by_id(prev_flat, prev_count,
                                                  curr_flat[i].id);
        if (!prev_entry) {
            deltas[dc].op = MD_A11Y_OP_ADD;
            deltas[dc].node = clone_node_shallow(curr_flat[i].node);
            dc++;
        } else if (nodes_differ(prev_entry->node, curr_flat[i].node)) {
            deltas[dc].op = MD_A11Y_OP_UPDATE;
            deltas[dc].node = clone_node_shallow(curr_flat[i].node);
            dc++;
        }
    }

    free(prev_flat);
    free(curr_flat);

    /* Replace snapshot */
    md_a11y_node_free(st->last_snapshot);
    st->last_snapshot = current;

    if (dc == 0) {
        free(deltas);
        *out_deltas = NULL;
        *out_count = 0;
        return 0;
    }

    *out_deltas = deltas;
    *out_count = dc;
    return 0;
}

static int axui_subscribe_changes(MdA11yCtx *ctx, MdA11yChangeCb cb,
                                  void *userdata) {
    (void)ctx; (void)cb; (void)userdata;
    /* Phase 2: register AXObserver for AXFocusedUIElementChanged,
     * AXValueChanged, AXUIElementDestroyed, etc.
     * For now, return -1 to indicate polling-only mode. */
    return -1;
}

static void axui_destroy(MdA11yCtx *ctx) {
    AXUIState *st = ctx->backend_data;
    if (!st) return;

    md_a11y_node_free(st->last_snapshot);
    free(st);
    ctx->backend_data = NULL;
}

/* ── Singleton vtable ────────────────────────────────────────── */

static const MdA11yBackend axui_backend = {
    .init              = axui_init,
    .get_tree          = axui_get_tree,
    .get_diff          = axui_get_diff,
    .subscribe_changes = axui_subscribe_changes,
    .destroy           = axui_destroy,
};

const MdA11yBackend *md_a11y_backend_create(void) {
    return &axui_backend;
}
