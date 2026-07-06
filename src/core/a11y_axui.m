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
#include <ctype.h>
#include <pthread.h>

/* Maximum tree depth to prevent infinite recursion */
#define MD_AX_MAX_DEPTH 32

/* Maximum children per node */
#define MD_AX_MAX_CHILDREN 256

/* Older SDKs do not expose the generic AXCreated name, but AX accepts
 * notification names as CFStrings. We still also register AXWindowCreated below
 * because that is the public macOS window-creation notification. */
#ifndef kAXCreatedNotification
#define kAXCreatedNotification CFSTR("AXCreated")
#endif

/* ── Backend-private state ───────────────────────────────────── */

typedef struct {
    AXUIElementRef     app_elem;
    AXObserverRef      observer;
    CFRunLoopSourceRef source;
} AXObserverRecord;

typedef struct {
    int                connected;
    uint64_t           next_id;
    MdA11yNode        *last_snapshot;
    pthread_mutex_t    lock;
    AXObserverRecord  *observers;
    size_t             observer_count;
    pthread_t          event_thread;
    int                event_thread_started;
    CFRunLoopRef       event_run_loop;
    int                stop_requested;
    MdA11yChangeCb     change_cb;
    void              *change_userdata;
    int                subscribed;
} AXUIState;

/* ── Helpers ─────────────────────────────────────────────────── */

static char *make_node_id(AXUIState *st) {
    char buf[32];
    snprintf(buf, sizeof(buf), "n%lu", (unsigned long)st->next_id++);
    return strdup(buf);
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

    /* Fallback: strip "AX" prefix and lowercase for unrecognized roles.
     * e.g. "AXSystemWide" → "systemwide", "AXBrowser" → "browser" */
    if (strncmp(ax_role, "AX", 2) == 0 && ax_role[2] != '\0') {
        static _Thread_local char fallback[128];
        const char *src = ax_role + 2;
        size_t i = 0;
        for (; src[i] && i < sizeof(fallback) - 1; i++)
            fallback[i] = (char)tolower((unsigned char)src[i]);
        fallback[i] = '\0';
        return fallback;
    }

    return ax_role;
}





/* ── Tree walking ────────────────────────────────────────────── */

/* Batch-fetch multiple AX attributes in one IPC round-trip.
 * Returns a CFArrayRef of attribute values (caller must CFRelease).
 * Missing/errored attributes appear as kCFNull in the array. */
static CFArrayRef ax_copy_multi(AXUIElementRef elem, CFArrayRef attrs) {
    CFArrayRef values = NULL;
    AXError err = AXUIElementCopyMultipleAttributeValues(elem, attrs,
                      0 /* options */, &values);
    if (err != kAXErrorSuccess) return NULL;
    return values;
}

/* Extract a CFStringRef from a batch result array at index, or NULL. */
static char *batch_get_string(CFArrayRef values, CFIndex idx) {
    if (!values || idx >= CFArrayGetCount(values)) return NULL;
    CFTypeRef val = CFArrayGetValueAtIndex(values, idx);
    if (!val || val == kCFNull) return NULL;
    if (CFGetTypeID(val) != CFStringGetTypeID()) return NULL;
    CFStringRef str = (CFStringRef)val;
    CFIndex len = CFStringGetMaximumSizeForEncoding(
        CFStringGetLength(str), kCFStringEncodingUTF8) + 1;
    char *buf = malloc((size_t)len);
    if (!buf) return NULL;
    if (!CFStringGetCString(str, buf, len, kCFStringEncodingUTF8)) {
        free(buf);
        return NULL;
    }
    return buf;
}

static MdA11yNode *walk_element(AXUIState *st, AXUIElementRef elem, int depth) {
    if (!elem || depth > MD_AX_MAX_DEPTH)
        return NULL;

    MdA11yNode *node = calloc(1, sizeof(MdA11yNode));
    if (!node) return NULL;

    node->id = make_node_id(st);

    /* Batch-request all scalar attributes in one IPC call.
     * Indices: 0=Role, 1=Title, 2=Description, 3=Value,
     *          4=Enabled, 5=Focused, 6=Selected, 7=Position, 8=Size */
    CFStringRef attr_keys[] = {
        kAXRoleAttribute,          /* 0 */
        kAXTitleAttribute,         /* 1 */
        kAXDescriptionAttribute,   /* 2 */
        kAXValueAttribute,         /* 3 */
        kAXEnabledAttribute,       /* 4 */
        kAXFocusedAttribute,       /* 5 */
        kAXSelectedAttribute,      /* 6 */
        kAXPositionAttribute,      /* 7 */
        kAXSizeAttribute,          /* 8 */
    };
    enum { AX_ROLE=0, AX_TITLE, AX_DESC, AX_VALUE,
           AX_ENABLED, AX_FOCUSED, AX_SELECTED, AX_POS, AX_SIZE,
           AX_ATTR_COUNT };
    CFArrayRef attr_names = CFArrayCreate(NULL, (const void **)attr_keys,
                                         AX_ATTR_COUNT,
                                         &kCFTypeArrayCallBacks);
    CFArrayRef vals = ax_copy_multi(elem, attr_names);
    CFRelease(attr_names);

    /* Role */
    char *raw_role = vals ? batch_get_string(vals, AX_ROLE) : NULL;
    const char *normalized = normalize_role(raw_role);
    node->role = strdup(normalized);
    free(raw_role);

    /* Label: try Title, then Description, then Value for text roles */
    node->label = vals ? batch_get_string(vals, AX_TITLE) : NULL;
    if (!node->label || node->label[0] == '\0') {
        free(node->label);
        node->label = vals ? batch_get_string(vals, AX_DESC) : NULL;
    }
    if (!node->label || node->label[0] == '\0') {
        if (node->role && (strcmp(node->role, "label") == 0 ||
                          strcmp(node->role, "text") == 0)) {
            free(node->label);
            node->label = vals ? batch_get_string(vals, AX_VALUE) : NULL;
        }
    }

    /* States — extract from batch results instead of per-attribute IPC */
    {
        char *states[8];
        int scount = 0;

        if (vals) {
            /* Enabled */
            CFTypeRef v = CFArrayGetValueAtIndex(vals, AX_ENABLED);
            if (v && v != kCFNull && CFGetTypeID(v) == CFBooleanGetTypeID()
                && CFBooleanGetValue((CFBooleanRef)v))
                states[scount++] = strdup("enabled");

            /* Focused */
            v = CFArrayGetValueAtIndex(vals, AX_FOCUSED);
            if (v && v != kCFNull && CFGetTypeID(v) == CFBooleanGetTypeID()
                && CFBooleanGetValue((CFBooleanRef)v))
                states[scount++] = strdup("focused");

            /* Selected */
            v = CFArrayGetValueAtIndex(vals, AX_SELECTED);
            if (v && v != kCFNull && CFGetTypeID(v) == CFBooleanGetTypeID()
                && CFBooleanGetValue((CFBooleanRef)v))
                states[scount++] = strdup("selected");

            /* Visible/showing if position exists */
            v = CFArrayGetValueAtIndex(vals, AX_POS);
            if (v && v != kCFNull) {
                states[scount++] = strdup("visible");
                states[scount++] = strdup("showing");
            }
        }

        if (scount > 0) {
            node->states = calloc((size_t)scount, sizeof(char *));
            if (node->states) {
                memcpy(node->states, states, (size_t)scount * sizeof(char *));
                node->state_count = scount;
            } else {
                for (int i = 0; i < scount; i++) free(states[i]);
            }
        }
    }

    /* Bounds — extract from batch position/size */
    if (vals) {
        CFTypeRef posValue = CFArrayGetValueAtIndex(vals, AX_POS);
        CFTypeRef sizeValue = CFArrayGetValueAtIndex(vals, AX_SIZE);
        if (posValue && posValue != kCFNull && sizeValue && sizeValue != kCFNull) {
            CGPoint pos;
            CGSize size;
            if (AXValueGetValue(posValue, kAXValueCGPointType, &pos) &&
                AXValueGetValue(sizeValue, kAXValueCGSizeType, &size)) {
                node->x = (int)pos.x;
                node->y = (int)pos.y;
                node->w = (int)size.width;
                node->h = (int)size.height;
            }
        }
    }

    if (vals) CFRelease(vals);

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

static int axui_get_tree_unlocked(MdA11yCtx *ctx, MdA11yNode **out_root);
static int axui_get_diff_unlocked(MdA11yCtx *ctx, MdA11yDelta **out_deltas,
                                  int *out_count);

/* ── Change subscriptions ────────────────────────────────────── */

static const CFStringRef axui_change_notifications[] = {
    kAXFocusedUIElementChangedNotification,
    kAXCreatedNotification,
    kAXWindowCreatedNotification,
    kAXUIElementDestroyedNotification,
    kAXValueChangedNotification,
};

static size_t axui_notification_count(void) {
    return sizeof(axui_change_notifications) / sizeof(axui_change_notifications[0]);
}

static const char *axui_ax_error_name(AXError err) {
    switch (err) {
    case kAXErrorSuccess: return "success";
    case kAXErrorFailure: return "failure";
    case kAXErrorIllegalArgument: return "illegal argument";
    case kAXErrorInvalidUIElement: return "invalid UI element";
    case kAXErrorInvalidUIElementObserver: return "invalid observer";
    case kAXErrorCannotComplete: return "cannot complete";
    case kAXErrorAttributeUnsupported: return "attribute unsupported";
    case kAXErrorActionUnsupported: return "action unsupported";
    case kAXErrorNotificationUnsupported: return "notification unsupported";
    case kAXErrorNotImplemented: return "not implemented";
    case kAXErrorNotificationAlreadyRegistered: return "notification already registered";
    case kAXErrorNotificationNotRegistered: return "notification not registered";
    case kAXErrorAPIDisabled: return "api disabled";
    case kAXErrorNoValue: return "no value";
    case kAXErrorParameterizedAttributeUnsupported: return "parameterized attribute unsupported";
    case kAXErrorNotEnoughPrecision: return "not enough precision";
    default: return "unknown";
    }
}

static void axui_release_observer_records(AXObserverRecord *records,
                                          size_t count) {
    if (!records) return;

    for (size_t i = 0; i < count; i++) {
        AXObserverRecord *rec = &records[i];
        if (rec->observer && rec->app_elem) {
            for (size_t n = 0; n < axui_notification_count(); n++)
                AXObserverRemoveNotification(rec->observer, rec->app_elem,
                                             axui_change_notifications[n]);
        }
        if (rec->observer) CFRelease(rec->observer);
        if (rec->app_elem) CFRelease(rec->app_elem);
    }

    free(records);
}

static void axui_observer_cb(AXObserverRef observer, AXUIElementRef element,
                             CFStringRef notification, void *refcon) {
    (void)observer;
    (void)element;
    (void)notification;

    @autoreleasepool {
        MdA11yCtx *ctx = refcon;
        if (!ctx) return;

        AXUIState *st = ctx->backend_data;
        if (!st) return;

        MdA11yChangeCb cb = NULL;
        void *cb_userdata = NULL;
        MdA11yDelta *deltas = NULL;
        int delta_count = 0;
        int diff_ret = -1;

        pthread_mutex_lock(&st->lock);
        if (st->subscribed && st->change_cb) {
            cb = st->change_cb;
            cb_userdata = st->change_userdata;
            diff_ret = axui_get_diff_unlocked(ctx, &deltas, &delta_count);
        }
        pthread_mutex_unlock(&st->lock);

        if (cb && diff_ret == 0)
            cb(deltas, delta_count, cb_userdata);

        md_a11y_delta_free(deltas, delta_count);
    }
}

static int axui_add_observer_record(AXObserverRecord **records,
                                    size_t *count,
                                    size_t *capacity,
                                    AXObserverRecord rec) {
    if (*count >= *capacity) {
        size_t new_capacity = (*capacity == 0) ? 8 : *capacity * 2;
        AXObserverRecord *new_records = realloc(*records,
            new_capacity * sizeof(AXObserverRecord));
        if (!new_records) return -1;
        *records = new_records;
        *capacity = new_capacity;
    }

    (*records)[(*count)++] = rec;
    return 0;
}

static int axui_create_observers(MdA11yCtx *ctx, AXObserverRecord **out_records,
                                 size_t *out_count) {
    if (!ctx || !out_records || !out_count) return -1;

    *out_records = NULL;
    *out_count = 0;

    AXObserverRecord *records = NULL;
    size_t count = 0;
    size_t capacity = 0;

    @autoreleasepool {
        NSArray<NSRunningApplication *> *apps =
            [[NSWorkspace sharedWorkspace] runningApplications];

        for (NSRunningApplication *app in apps) {
            if (app.activationPolicy != NSApplicationActivationPolicyRegular &&
                app.activationPolicy != NSApplicationActivationPolicyAccessory) {
                continue;
            }

            pid_t pid = app.processIdentifier;
            AXUIElementRef app_elem = AXUIElementCreateApplication(pid);
            if (!app_elem) continue;

            AXObserverRef observer = NULL;
            AXError err = AXObserverCreate(pid, axui_observer_cb, &observer);
            if (err != kAXErrorSuccess || !observer) {
                fprintf(stderr,
                        "a11y_axui: failed to create AXObserver for pid %d: %s\n",
                        (int)pid, axui_ax_error_name(err));
                CFRelease(app_elem);
                continue;
            }

            int registered = 0;
            for (size_t n = 0; n < axui_notification_count(); n++) {
                err = AXObserverAddNotification(observer, app_elem,
                                                axui_change_notifications[n],
                                                ctx);
                if (err == kAXErrorSuccess ||
                    err == kAXErrorNotificationAlreadyRegistered) {
                    registered++;
                    continue;
                }

                /* Notification support varies by target app and by the
                 * notification name (notably generic AXCreated). Unsupported
                 * notifications are non-fatal as long as at least one useful
                 * notification registered for this app. */
                if (err != kAXErrorNotificationUnsupported &&
                    err != kAXErrorNotImplemented) {
                    fprintf(stderr,
                            "a11y_axui: failed to register AX notification for pid %d: %s\n",
                            (int)pid, axui_ax_error_name(err));
                }
            }

            if (registered == 0) {
                CFRelease(observer);
                CFRelease(app_elem);
                continue;
            }

            AXObserverRecord rec = {
                .app_elem = app_elem,
                .observer = observer,
                .source = AXObserverGetRunLoopSource(observer),
            };

            if (!rec.source || axui_add_observer_record(&records, &count,
                                                        &capacity, rec) != 0) {
                for (size_t n = 0; n < axui_notification_count(); n++)
                    AXObserverRemoveNotification(observer, app_elem,
                                                 axui_change_notifications[n]);
                CFRelease(observer);
                CFRelease(app_elem);
                axui_release_observer_records(records, count);
                return -1;
            }
        }
    }

    if (count == 0) {
        free(records);
        return -1;
    }

    *out_records = records;
    *out_count = count;
    return 0;
}

static void *axui_event_loop_thread(void *data) {
    AXUIState *st = data;
    if (!st) return NULL;

    @autoreleasepool {
        CFRunLoopRef run_loop = CFRunLoopGetCurrent();
        CFRetain(run_loop);

        pthread_mutex_lock(&st->lock);
        st->event_run_loop = run_loop;
        for (size_t i = 0; i < st->observer_count; i++) {
            if (st->observers[i].source) {
                CFRunLoopAddSource(run_loop, st->observers[i].source,
                                   kCFRunLoopDefaultMode);
            }
        }
        pthread_mutex_unlock(&st->lock);

        for (;;) {
            pthread_mutex_lock(&st->lock);
            int stop = st->stop_requested;
            pthread_mutex_unlock(&st->lock);
            if (stop) break;

            @autoreleasepool {
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, true);
            }
        }

        pthread_mutex_lock(&st->lock);
        for (size_t i = 0; i < st->observer_count; i++) {
            if (st->observers[i].source) {
                CFRunLoopRemoveSource(run_loop, st->observers[i].source,
                                      kCFRunLoopDefaultMode);
            }
        }
        st->event_run_loop = NULL;
        pthread_mutex_unlock(&st->lock);

        CFRelease(run_loop);
    }

    return NULL;
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

    pthread_mutex_init(&st->lock, NULL);
    st->connected = 1;
    ctx->backend_data = st;
    return 0;
}

static int axui_get_tree_unlocked(MdA11yCtx *ctx, MdA11yNode **out_root) {
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

static int axui_get_tree(MdA11yCtx *ctx, MdA11yNode **out_root) {
    AXUIState *st = ctx ? ctx->backend_data : NULL;
    if (!st) return -1;

    pthread_mutex_lock(&st->lock);
    int ret = axui_get_tree_unlocked(ctx, out_root);
    pthread_mutex_unlock(&st->lock);
    return ret;
}

static int axui_get_diff_unlocked(MdA11yCtx *ctx, MdA11yDelta **out_deltas,
                                  int *out_count) {
    AXUIState *st = ctx->backend_data;
    if (!st || !out_deltas || !out_count) return -1;

    *out_deltas = NULL;
    *out_count = 0;

    MdA11yNode *current = NULL;
    if (axui_get_tree_unlocked(ctx, &current) != 0 || !current)
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

static int axui_get_diff(MdA11yCtx *ctx, MdA11yDelta **out_deltas,
                         int *out_count) {
    AXUIState *st = ctx ? ctx->backend_data : NULL;
    if (!st) return -1;

    pthread_mutex_lock(&st->lock);
    int ret = axui_get_diff_unlocked(ctx, out_deltas, out_count);
    pthread_mutex_unlock(&st->lock);
    return ret;
}

static int axui_subscribe_changes(MdA11yCtx *ctx, MdA11yChangeCb cb,
                                  void *userdata) {
    AXUIState *st = ctx ? ctx->backend_data : NULL;
    if (!st || !st->connected || !cb) return -1;

    pthread_mutex_lock(&st->lock);
    if (st->subscribed) {
        st->change_cb = cb;
        st->change_userdata = userdata;
        pthread_mutex_unlock(&st->lock);
        return 0;
    }
    pthread_mutex_unlock(&st->lock);

    if (!AXIsProcessTrusted()) {
        fprintf(stderr,
                "a11y_axui: accessibility permission required for AXObserver subscriptions\n");
        return -1;
    }

    /* The AXObserver API is scoped to one target application PID. The backend
     * snapshots the whole desktop, so subscribe by creating one observer for
     * each GUI app currently running. Apps launched after this call are not
     * observed until the caller recreates the subscription or an observed app
     * emits another event that causes a full-desktop delta snapshot. */
    AXObserverRecord *records = NULL;
    size_t record_count = 0;
    if (axui_create_observers(ctx, &records, &record_count) != 0) {
        fprintf(stderr,
                "a11y_axui: failed to create any AXObserver subscriptions\n");
        return -1;
    }

    pthread_mutex_lock(&st->lock);
    if (st->subscribed) {
        st->change_cb = cb;
        st->change_userdata = userdata;
        pthread_mutex_unlock(&st->lock);
        axui_release_observer_records(records, record_count);
        return 0;
    }

    st->observers = records;
    st->observer_count = record_count;
    st->change_cb = cb;
    st->change_userdata = userdata;
    st->stop_requested = 0;

    /* Seed the diff baseline so the first notification emits real deltas
     * rather than being consumed as the initial snapshot. */
    if (!st->last_snapshot) {
        MdA11yNode *initial = NULL;
        if (axui_get_tree_unlocked(ctx, &initial) == 0)
            st->last_snapshot = initial;
    }

    st->subscribed = 1;
    pthread_mutex_unlock(&st->lock);

    int err = pthread_create(&st->event_thread, NULL,
                             axui_event_loop_thread, st);
    if (err != 0) {
        pthread_mutex_lock(&st->lock);
        st->subscribed = 0;
        st->change_cb = NULL;
        st->change_userdata = NULL;
        AXObserverRecord *cleanup_records = st->observers;
        size_t cleanup_count = st->observer_count;
        st->observers = NULL;
        st->observer_count = 0;
        pthread_mutex_unlock(&st->lock);

        axui_release_observer_records(cleanup_records, cleanup_count);
        return -1;
    }

    pthread_mutex_lock(&st->lock);
    st->event_thread_started = 1;
    pthread_mutex_unlock(&st->lock);

    return 0;
}

static void axui_destroy(MdA11yCtx *ctx) {
    AXUIState *st = ctx->backend_data;
    if (!st) return;

    pthread_t event_thread;
    int join_thread = 0;
    CFRunLoopRef run_loop = NULL;

    pthread_mutex_lock(&st->lock);
    st->connected = 0;
    st->subscribed = 0;
    st->change_cb = NULL;
    st->change_userdata = NULL;
    st->stop_requested = 1;
    if (st->event_run_loop) {
        run_loop = st->event_run_loop;
        CFRetain(run_loop);
    }
    if (st->event_thread_started) {
        event_thread = st->event_thread;
        join_thread = 1;
    }
    pthread_mutex_unlock(&st->lock);

    if (run_loop) {
        CFRunLoopStop(run_loop);
        CFRelease(run_loop);
    }

    if (join_thread && !pthread_equal(pthread_self(), event_thread))
        pthread_join(event_thread, NULL);

    pthread_mutex_lock(&st->lock);
    AXObserverRecord *records = st->observers;
    size_t record_count = st->observer_count;
    MdA11yNode *last_snapshot = st->last_snapshot;
    st->observers = NULL;
    st->observer_count = 0;
    st->last_snapshot = NULL;
    pthread_mutex_unlock(&st->lock);

    axui_release_observer_records(records, record_count);
    md_a11y_node_free(last_snapshot);
    pthread_mutex_destroy(&st->lock);
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
