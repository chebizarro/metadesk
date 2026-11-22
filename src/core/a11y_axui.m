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
#include "log.h"
#define MD_LOG_TAG "a11y_axui"

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
    pthread_mutex_t    lock;
    AXObserverRecord  *observers;
    size_t             observer_count;
    pthread_t          event_thread;
    int                event_thread_started;
    CFRunLoopRef       event_run_loop;
    int                stop_requested;   /* set before CFRunLoopRun races */
    int                subscribed;       /* observers registered; the callback
                                          * itself lives on the MdA11yCtx */
} AXUIState;

/* ── Helpers ─────────────────────────────────────────────────── */

static char *make_node_id(AXUIState *st) {
    char buf[32];
    snprintf(buf, sizeof(buf), "n%lu", (unsigned long)st->next_id++);
    return strdup(buf);
}




/* Typed AX mappings: no raw native role escapes into a tree. */
static const struct { const char *ax_role; enum MdA11yRole role; } ax_roles[] = {
    { "AXApplication", MD_A11Y_ROLE_APPLICATION },
    { "AXWindow", MD_A11Y_ROLE_WINDOW },
    { "AXButton", MD_A11Y_ROLE_BUTTON },
    { "AXCheckBox", MD_A11Y_ROLE_CHECK_BOX },
    { "AXRadioButton", MD_A11Y_ROLE_RADIO_BUTTON },
    { "AXTextField", MD_A11Y_ROLE_TEXT },
    { "AXTextArea", MD_A11Y_ROLE_TEXT },
    { "AXStaticText", MD_A11Y_ROLE_LABEL },
    { "AXImage", MD_A11Y_ROLE_IMAGE },
    { "AXGroup", MD_A11Y_ROLE_PANEL },
    { "AXList", MD_A11Y_ROLE_LIST },
    { "AXTable", MD_A11Y_ROLE_TABLE },
    { "AXRow", MD_A11Y_ROLE_TABLE_ROW },
    { "AXColumn", MD_A11Y_ROLE_TABLE_COLUMN },
    { "AXCell", MD_A11Y_ROLE_TABLE_CELL },
    { "AXScrollArea", MD_A11Y_ROLE_SCROLL_PANE },
    { "AXScrollBar", MD_A11Y_ROLE_SCROLL_BAR },
    { "AXSlider", MD_A11Y_ROLE_SLIDER },
    { "AXMenuBar", MD_A11Y_ROLE_MENU },
    { "AXMenu", MD_A11Y_ROLE_MENU },
    { "AXMenuItem", MD_A11Y_ROLE_MENU_ITEM },
    { "AXToolbar", MD_A11Y_ROLE_TOOL_BAR },
    { "AXTabGroup", MD_A11Y_ROLE_PAGE_TAB_LIST },
    { "AXTab", MD_A11Y_ROLE_TAB },
    { "AXComboBox", MD_A11Y_ROLE_COMBO_BOX },
    { "AXPopUpButton", MD_A11Y_ROLE_COMBO_BOX },
    { "AXProgressIndicator", MD_A11Y_ROLE_PROGRESS_BAR },
    { "AXLink", MD_A11Y_ROLE_LINK },
    { "AXWebArea", MD_A11Y_ROLE_DOCUMENT_WEB },
    { "AXSheet", MD_A11Y_ROLE_DIALOG },
    { "AXDialog", MD_A11Y_ROLE_DIALOG },
    { "AXSplitGroup", MD_A11Y_ROLE_SPLIT_PANE },
    { "AXOutline", MD_A11Y_ROLE_TREE },
    { "AXOutlineRow", MD_A11Y_ROLE_TREE_ITEM },
    { "AXHeading", MD_A11Y_ROLE_HEADING },
    { "AXMenuBarItem", MD_A11Y_ROLE_MENU_ITEM },
    { "AXValueIndicator", MD_A11Y_ROLE_LABEL },
    { "AXSplitter", MD_A11Y_ROLE_SEPARATOR },
    { "AXDisclosureTriangle", MD_A11Y_ROLE_TOGGLE_BUTTON },
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

/* Used by the native drift test; iterates the actual production mapping. */
size_t md_a11y_axui_role_mapping_count(void) {
    return sizeof(ax_roles) / sizeof(ax_roles[0]);
}
enum MdA11yRole md_a11y_axui_role_mapping_at(size_t index) {
    return index < md_a11y_axui_role_mapping_count()
        ? ax_roles[index].role : MD_A11Y_ROLE_UNKNOWN;
}
/* Bounded process-lifetime set. Once full, suppress new diagnostics rather
 * than logging repeatedly; store complete names to avoid truncation collisions. */
static void log_unmapped_role_once(const char *role) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static char *seen[32];
    static size_t count;
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(seen[i], role) == 0) {
            pthread_mutex_unlock(&lock);
            return;
        }
    }
    if (count < sizeof(seen) / sizeof(seen[0])) {
        char *copy = strdup(role);
        if (copy) {
            seen[count++] = copy;
            MD_LOG_I("unmapped AX role: %s", role);
        }
    }
    pthread_mutex_unlock(&lock);
}

static const char *normalize_role(const char *ax_role) {
    if (ax_role) {
        for (size_t i = 0; i < md_a11y_axui_role_mapping_count(); i++)
            if (strcmp(ax_role, ax_roles[i].ax_role) == 0)
                return md_a11y_role_name(ax_roles[i].role);
        log_unmapped_role_once(ax_role);
    }
    return md_a11y_role_name(MD_A11Y_ROLE_UNKNOWN);
}

/* Native test seams exercise the production string mapping without CF. */
const char *md_a11y_axui_normalize_role(const char *ax_role) {
    return normalize_role(ax_role);
}
const char *md_a11y_axui_native_role_at(size_t index) {
    return index < md_a11y_axui_role_mapping_count() ? ax_roles[index].ax_role : NULL;
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

static char *axui_element_id(AXUIElementRef elem) {
    pid_t pid = 0;
    if (!elem || AXUIElementGetPid(elem, &pid) != kAXErrorSuccess) return NULL;
    char id[64];
    snprintf(id, sizeof(id), "ax%ld:%lx", (long)pid, (unsigned long)CFHash(elem));
    return strdup(id);
}

static AXUIElementRef axui_element_at_point(MdA11yCtx *ctx, int x, int y) {
    AXUIState *st = ctx ? ctx->backend_data : NULL;
    if (!st || !st->connected || !AXIsProcessTrusted()) return NULL;
    AXUIElementRef desktop = AXUIElementCreateSystemWide();
    if (!desktop) return NULL;
    AXUIElementRef hit = NULL;
    AXError error = AXUIElementCopyElementAtPosition(desktop, x, y, &hit);
    CFRelease(desktop);
    if (error != kAXErrorSuccess) {
        if (hit) CFRelease(hit);
        return NULL;
    }
    return hit;
}

static char *axui_hit_test(MdA11yCtx *ctx, int x, int y) {
    AXUIElementRef hit = axui_element_at_point(ctx, x, y);
    char *id = axui_element_id(hit);
    if (hit) CFRelease(hit);
    return id;
}

static int axui_click_target(MdA11yCtx *ctx, const char *id, int x, int y) {
    AXUIElementRef hit = axui_element_at_point(ctx, x, y);
    char *hit_id = axui_element_id(hit);
    int result = -1;
    if (hit_id && id && strcmp(hit_id, id) == 0 &&
        AXUIElementPerformAction(hit, kAXPressAction) == kAXErrorSuccess)
        result = 0;
    free(hit_id);
    if (hit) CFRelease(hit);
    return result;
}

static MdA11yNode *walk_element(AXUIState *st, AXUIElementRef elem, int depth) {
    if (!elem || depth > MD_AX_MAX_DEPTH)
        return NULL;

    MdA11yNode *node = calloc(1, sizeof(MdA11yNode));
    if (!node) return NULL;

    node->id = make_node_id(st);

    char *stable_id = axui_element_id(elem);
    if (stable_id) {
        free(node->id);
        node->id = stable_id;
    }

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
        enum MdA11yRole role = md_a11y_role_lookup(node->role);
        if (role == MD_A11Y_ROLE_LABEL || role == MD_A11Y_ROLE_TEXT) {
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
                states[scount++] = strdup(md_a11y_state_name(MD_A11Y_STATE_ENABLED));

            /* Focused */
            v = CFArrayGetValueAtIndex(vals, AX_FOCUSED);
            if (v && v != kCFNull && CFGetTypeID(v) == CFBooleanGetTypeID()
                && CFBooleanGetValue((CFBooleanRef)v))
                states[scount++] = strdup(md_a11y_state_name(MD_A11Y_STATE_FOCUSED));

            /* Selected */
            v = CFArrayGetValueAtIndex(vals, AX_SELECTED);
            if (v && v != kCFNull && CFGetTypeID(v) == CFBooleanGetTypeID()
                && CFBooleanGetValue((CFBooleanRef)v))
                states[scount++] = strdup(md_a11y_state_name(MD_A11Y_STATE_SELECTED));

            /* Visible/showing if position exists */
            v = CFArrayGetValueAtIndex(vals, AX_POS);
            if (v && v != kCFNull) {
                states[scount++] = strdup(md_a11y_state_name(MD_A11Y_STATE_VISIBLE));
                states[scount++] = strdup(md_a11y_state_name(MD_A11Y_STATE_SHOWING));
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

        md_a11y_change_dispatch(ctx);
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
                MD_LOG_E("failed to create AXObserver for pid %d: %s", (int)pid, axui_ax_error_name(err));
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
                    MD_LOG_E("failed to register AX notification for pid %d: %s", (int)pid, axui_ax_error_name(err));
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
        if (st->stop_requested) {
            /* destroy() won the race — it never saw our run loop */
            pthread_mutex_unlock(&st->lock);
            CFRelease(run_loop);
            return NULL;
        }
        st->event_run_loop = run_loop;
        for (size_t i = 0; i < st->observer_count; i++) {
            if (st->observers[i].source) {
                CFRunLoopAddSource(run_loop, st->observers[i].source,
                                   kCFRunLoopDefaultMode);
            }
        }
        pthread_mutex_unlock(&st->lock);

        /* Event-driven: teardown stops this by enqueuing a CFRunLoopStop
         * block on this loop (see axui_teardown_subscription) — no polling
         * wake-up needed, and the stop is honoured even if it was enqueued
         * between the publish above and this call. */
        CFRunLoopRun();

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
    /* Fail fast when accessibility permission is missing: without trust,
     * every AX IPC call blocks for its ~6 s timeout, so a walk of the
     * desktop would grind for minutes instead of failing. */
    if (!AXIsProcessTrusted()) {
        MD_LOG_E("accessibility permission not granted — enable this app in System Settings > Privacy & Security > Accessibility");
        return -1;
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

    root->id = strdup("desktop");
    root->role = strdup(md_a11y_role_name(MD_A11Y_ROLE_DESKTOP));
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

static int axui_subscribe_changes(MdA11yCtx *ctx, MdA11yChangeCb cb,
                                  void *userdata) {
    AXUIState *st = ctx ? ctx->backend_data : NULL;
    if (!st || !st->connected || !cb) return -1;
    (void)userdata; /* callback + userdata are stored on ctx by a11y.c */

    pthread_mutex_lock(&st->lock);
    if (st->subscribed) {
        /* Observers already registered; a11y.c swapped the callback. */
        pthread_mutex_unlock(&st->lock);
        return 0;
    }
    pthread_mutex_unlock(&st->lock);

    if (!AXIsProcessTrusted()) {
        MD_LOG_I("accessibility permission required for AXObserver subscriptions");
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
        MD_LOG_E("failed to create any AXObserver subscriptions");
        return -1;
    }

    pthread_mutex_lock(&st->lock);
    if (st->subscribed) {
        pthread_mutex_unlock(&st->lock);
        axui_release_observer_records(records, record_count);
        return 0;
    }

    st->observers = records;
    st->observer_count = record_count;

    st->subscribed = 1;
    st->stop_requested = 0;  /* thread handshake starts clear */
    pthread_mutex_unlock(&st->lock);

    int err = pthread_create(&st->event_thread, NULL,
                             axui_event_loop_thread, st);
    if (err != 0) {
        pthread_mutex_lock(&st->lock);
        st->subscribed = 0;
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

/* Single teardown path shared by unsubscribe and destroy. Stops the
 * backend-owned run loop thread (which removes its CFRunLoop sources on
 * the way out), joins it, then removes every AX notification and releases
 * the observers. The user callback is not touched here: a11y.c has already
 * cleared and drained it before calling us.
 *
 * The observers are scheduled only on the backend's own thread, never on
 * the caller's run loop, so joining that thread is what guarantees no
 * AXObserver callback is still executing when this returns. If this were
 * ever reached from the event thread itself (unsubscribe inside the
 * callback — documented as unsupported) the join is skipped to avoid
 * self-deadlock. Idempotent. */
static void axui_teardown_subscription(AXUIState *st) {
    pthread_t event_thread;
    int join_thread = 0;
    CFRunLoopRef run_loop = NULL;

    pthread_mutex_lock(&st->lock);
    st->subscribed = 0;
    /* Order matters: stop_requested is set under the same lock the
     * thread uses to publish its run loop, so the stop can never be
     * lost to a thread that has not started running yet. */
    st->stop_requested = 1;
    if (st->event_run_loop) {
        run_loop = st->event_run_loop;
        CFRetain(run_loop);
    }
    if (st->event_thread_started) {
        event_thread = st->event_thread;
        join_thread = 1;
        st->event_thread_started = 0;
    }
    pthread_mutex_unlock(&st->lock);

    if (run_loop) {
        /* CFRunLoopStop() only takes effect on a loop that is currently
         * running (it flags the current mode), so a stop issued between the
         * thread publishing its loop and entering CFRunLoopRun() would be
         * lost and the join below would hang. Enqueue the stop as a block
         * instead: the run loop keeps it and performs it on its next pass in
         * this mode, whether the loop is already running or about to start,
         * and CFRunLoopStop from inside the loop is never lost. */
        CFRunLoopPerformBlock(run_loop, kCFRunLoopDefaultMode, ^{
            CFRunLoopStop(CFRunLoopGetCurrent());
        });
        CFRunLoopWakeUp(run_loop);
        CFRelease(run_loop);
    }

    if (join_thread && !pthread_equal(pthread_self(), event_thread))
        pthread_join(event_thread, NULL);

    pthread_mutex_lock(&st->lock);
    AXObserverRecord *records = st->observers;
    size_t record_count = st->observer_count;
    st->observers = NULL;
    st->observer_count = 0;
    pthread_mutex_unlock(&st->lock);

    axui_release_observer_records(records, record_count);
}

static void axui_unsubscribe_changes(MdA11yCtx *ctx) {
    AXUIState *st = ctx ? ctx->backend_data : NULL;
    if (!st) return;
    axui_teardown_subscription(st);
}

static void axui_destroy(MdA11yCtx *ctx) {
    AXUIState *st = ctx->backend_data;
    if (!st) return;

    pthread_mutex_lock(&st->lock);
    st->connected = 0;
    pthread_mutex_unlock(&st->lock);

    axui_teardown_subscription(st);
    pthread_mutex_destroy(&st->lock);
    free(st);
    ctx->backend_data = NULL;
}

/* ── Singleton vtable ────────────────────────────────────────── */

static bool axui_is_connected(const MdA11yCtx *ctx) {
    const AXUIState *st = ctx->backend_data;
    /* Tree walks need trust, not observers: change subscriptions are optional. */
    return st && st->connected && AXIsProcessTrusted();
}

static const MdA11yBackend axui_backend = {
    .is_connected      = axui_is_connected,
    .hit_test          = axui_hit_test,
    .click_target      = axui_click_target,
    .init              = axui_init,
    .get_tree          = axui_get_tree,
    .subscribe_changes   = axui_subscribe_changes,
    .unsubscribe_changes = axui_unsubscribe_changes,
    .destroy             = axui_destroy,
};

const MdA11yBackend *md_a11y_backend_create(void) {
    return &axui_backend;
}
