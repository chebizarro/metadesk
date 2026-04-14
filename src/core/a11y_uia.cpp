/*
 * metadesk — a11y_uia.cpp
 * Windows accessibility backend: UI Automation (spec §2.3.2).
 *
 * Uses Microsoft UI Automation COM API to:
 *   1. CoCreateInstance IUIAutomation
 *   2. GetRootElement → walk tree via TreeWalker
 *   3. Extract: ControlType, Name, BoundingRectangle, IsEnabled, etc.
 *   4. Build MdA11yNode tree
 *   5. Compute deltas by comparing current vs previous snapshot
 *   6. (Phase 2) Event handlers for live changes
 *
 * Requires Windows 7+.
 * UIA control types are mapped to platform-neutral role strings.
 */
extern "C" {
#include "a11y.h"
}

#ifdef _WIN32

#include <UIAutomation.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>

/* Maximum tree depth */
#define MD_UIA_MAX_DEPTH     32
#define MD_UIA_MAX_CHILDREN  256

/* ── Backend-private state ───────────────────────────────────── */

struct UIAState {
    IUIAutomation      *automation;
    IUIAutomationTreeWalker *walker;
    int                 connected;
    uint64_t            next_id;
    MdA11yNode         *last_snapshot;
};

/* ── Helpers ─────────────────────────────────────────────────── */

static char *make_node_id(UIAState *st) {
    char buf[32];
    snprintf(buf, sizeof(buf), "n%llu", (unsigned long long)st->next_id++);
    return _strdup(buf);
}

/* Convert BSTR to C string. Caller frees. */
static char *bstr_to_cstr(BSTR bstr) {
    if (!bstr) return nullptr;
    int len = WideCharToMultiByte(CP_UTF8, 0, bstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return nullptr;
    char *str = (char *)malloc((size_t)len);
    if (!str) return nullptr;
    WideCharToMultiByte(CP_UTF8, 0, bstr, -1, str, len, nullptr, nullptr);
    return str;
}

/* Map UIA ControlTypeId to platform-neutral role string */
static const char *control_type_to_role(CONTROLTYPEID ctid) {
    switch (ctid) {
    case UIA_ButtonControlTypeId:        return "push button";
    case UIA_CheckBoxControlTypeId:      return "check box";
    case UIA_RadioButtonControlTypeId:   return "radio button";
    case UIA_ComboBoxControlTypeId:      return "combo box";
    case UIA_EditControlTypeId:          return "text";
    case UIA_ListControlTypeId:          return "list";
    case UIA_ListItemControlTypeId:      return "list item";
    case UIA_MenuControlTypeId:          return "menu";
    case UIA_MenuBarControlTypeId:       return "menu bar";
    case UIA_MenuItemControlTypeId:      return "menu item";
    case UIA_TabControlTypeId:           return "page tab list";
    case UIA_TabItemControlTypeId:       return "page tab";
    case UIA_TreeControlTypeId:          return "tree";
    case UIA_TreeItemControlTypeId:      return "tree item";
    case UIA_SliderControlTypeId:        return "slider";
    case UIA_ScrollBarControlTypeId:     return "scroll bar";
    case UIA_ProgressBarControlTypeId:   return "progress bar";
    case UIA_StatusBarControlTypeId:     return "status bar";
    case UIA_ToolBarControlTypeId:       return "tool bar";
    case UIA_WindowControlTypeId:        return "window";
    case UIA_PaneControlTypeId:          return "panel";
    case UIA_GroupControlTypeId:         return "panel";
    case UIA_TextControlTypeId:          return "label";
    case UIA_ImageControlTypeId:         return "image";
    case UIA_HyperlinkControlTypeId:     return "link";
    case UIA_DocumentControlTypeId:      return "document";
    case UIA_TableControlTypeId:         return "table";
    case UIA_HeaderControlTypeId:        return "heading";
    case UIA_DataGridControlTypeId:      return "table";
    case UIA_DataItemControlTypeId:      return "table cell";
    case UIA_SplitButtonControlTypeId:   return "push button";
    case UIA_SpinnerControlTypeId:       return "spin button";
    case UIA_ToolTipControlTypeId:       return "tool tip";
    default:                             return "unknown";
    }
}

/* Extract states from UIA element properties */
static void extract_states(IUIAutomationElement *elem, MdA11yNode *node) {
    char *states[8];
    int count = 0;

    BOOL boolVal;

    if (SUCCEEDED(elem->get_CurrentIsEnabled(&boolVal)) && boolVal)
        states[count++] = _strdup("enabled");

    if (SUCCEEDED(elem->get_CurrentHasKeyboardFocus(&boolVal)) && boolVal)
        states[count++] = _strdup("focused");

    if (SUCCEEDED(elem->get_CurrentIsKeyboardFocusable(&boolVal)) && boolVal)
        states[count++] = _strdup("focusable");

    if (SUCCEEDED(elem->get_CurrentIsOffscreen(&boolVal)) && !boolVal) {
        states[count++] = _strdup("visible");
        states[count++] = _strdup("showing");
    }

    if (count > 0) {
        node->states = (char **)calloc((size_t)count, sizeof(char *));
        if (node->states) {
            memcpy(node->states, states, (size_t)count * sizeof(char *));
            node->state_count = count;
        } else {
            for (int i = 0; i < count; i++) free(states[i]);
        }
    }
}

/* ── Tree walking ────────────────────────────────────────────── */

static MdA11yNode *walk_element(UIAState *st, IUIAutomationElement *elem, int depth) {
    if (!elem || depth > MD_UIA_MAX_DEPTH)
        return nullptr;

    MdA11yNode *node = (MdA11yNode *)calloc(1, sizeof(MdA11yNode));
    if (!node) return nullptr;

    node->id = make_node_id(st);

    /* Role (ControlType) */
    CONTROLTYPEID ctid = 0;
    elem->get_CurrentControlType(&ctid);
    node->role = _strdup(control_type_to_role(ctid));

    /* Label (Name property) */
    BSTR bstrName = nullptr;
    if (SUCCEEDED(elem->get_CurrentName(&bstrName)) && bstrName) {
        node->label = bstr_to_cstr(bstrName);
        SysFreeString(bstrName);
    }

    /* States */
    extract_states(elem, node);

    /* Bounding rectangle */
    RECT rect;
    if (SUCCEEDED(elem->get_CurrentBoundingRectangle(&rect))) {
        node->x = rect.left;
        node->y = rect.top;
        node->w = rect.right - rect.left;
        node->h = rect.bottom - rect.top;
    }

    /* Walk children via TreeWalker */
    IUIAutomationElement *child = nullptr;
    HRESULT hr = st->walker->GetFirstChildElement(elem, &child);
    if (SUCCEEDED(hr) && child) {
        /* Count and collect children */
        MdA11yNode *children[MD_UIA_MAX_CHILDREN];
        int childCount = 0;

        while (child && childCount < MD_UIA_MAX_CHILDREN) {
            MdA11yNode *childNode = walk_element(st, child, depth + 1);
            if (childNode)
                children[childCount++] = childNode;

            IUIAutomationElement *next = nullptr;
            hr = st->walker->GetNextSiblingElement(child, &next);
            child->Release();
            child = (SUCCEEDED(hr)) ? next : nullptr;
        }

        if (childCount > 0) {
            node->children = (MdA11yNode **)calloc((size_t)childCount, sizeof(MdA11yNode *));
            if (node->children) {
                memcpy(node->children, children, (size_t)childCount * sizeof(MdA11yNode *));
                node->child_count = childCount;
            } else {
                for (int i = 0; i < childCount; i++)
                    md_a11y_node_free(children[i]);
            }
        }
    }

    return node;
}

/* ── Delta computation (same algorithm as other backends) ────── */

struct FlatEntry {
    const char        *id;
    const MdA11yNode  *node;
};

static void flatten_tree(const MdA11yNode *node, FlatEntry **entries,
                         int *count, int *capacity) {
    if (!node) return;

    if (*count >= *capacity) {
        *capacity = (*capacity == 0) ? 64 : *capacity * 2;
        *entries = (FlatEntry *)realloc(*entries, (size_t)*capacity * sizeof(FlatEntry));
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
    if (!id) return nullptr;
    for (int i = 0; i < count; i++) {
        if (entries[i].id && strcmp(entries[i].id, id) == 0)
            return &entries[i];
    }
    return nullptr;
}

static bool nodes_differ(const MdA11yNode *a, const MdA11yNode *b) {
    if (!a || !b) return true;
    if ((a->role == nullptr) != (b->role == nullptr)) return true;
    if (a->role && b->role && strcmp(a->role, b->role) != 0) return true;
    if ((a->label == nullptr) != (b->label == nullptr)) return true;
    if (a->label && b->label && strcmp(a->label, b->label) != 0) return true;
    if (a->x != b->x || a->y != b->y || a->w != b->w || a->h != b->h)
        return true;
    if (a->state_count != b->state_count) return true;
    return false;
}

static MdA11yNode *clone_node_shallow(const MdA11yNode *src) {
    if (!src) return nullptr;

    MdA11yNode *dst = (MdA11yNode *)calloc(1, sizeof(MdA11yNode));
    if (!dst) return nullptr;

    if (src->id)    dst->id    = _strdup(src->id);
    if (src->role)  dst->role  = _strdup(src->role);
    if (src->label) dst->label = _strdup(src->label);
    dst->x = src->x;
    dst->y = src->y;
    dst->w = src->w;
    dst->h = src->h;

    if (src->state_count > 0 && src->states) {
        dst->states = (char **)calloc((size_t)src->state_count, sizeof(char *));
        if (dst->states) {
            dst->state_count = src->state_count;
            for (int i = 0; i < src->state_count; i++) {
                if (src->states[i])
                    dst->states[i] = _strdup(src->states[i]);
            }
        }
    }

    return dst;
}

/* ── Vtable implementation ───────────────────────────────────── */

static int uia_init(MdA11yCtx *ctx) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "a11y_uia: CoInitializeEx failed: 0x%08lx\n", hr);
        return -1;
    }

    auto *st = (UIAState *)calloc(1, sizeof(UIAState));
    if (!st) return -1;

    hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                          CLSCTX_INPROC_SERVER,
                          __uuidof(IUIAutomation),
                          (void **)&st->automation);
    if (FAILED(hr) || !st->automation) {
        fprintf(stderr, "a11y_uia: failed to create IUIAutomation: 0x%08lx\n", hr);
        free(st);
        return -1;
    }

    /* Create a content tree walker (skips raw/control elements) */
    hr = st->automation->get_ContentViewWalker(&st->walker);
    if (FAILED(hr) || !st->walker) {
        fprintf(stderr, "a11y_uia: failed to get ContentViewWalker\n");
        st->automation->Release();
        free(st);
        return -1;
    }

    st->connected = 1;
    ctx->backend_data = st;
    return 0;
}

static int uia_get_tree(MdA11yCtx *ctx, MdA11yNode **out_root) {
    auto *st = (UIAState *)ctx->backend_data;
    if (!st || !st->connected || !out_root) return -1;

    st->next_id = 0;

    /* Get the root element (desktop) */
    IUIAutomationElement *rootElem = nullptr;
    HRESULT hr = st->automation->GetRootElement(&rootElem);
    if (FAILED(hr) || !rootElem) return -1;

    /* Create root node */
    MdA11yNode *root = (MdA11yNode *)calloc(1, sizeof(MdA11yNode));
    if (!root) {
        rootElem->Release();
        return -1;
    }

    root->id = make_node_id(st);
    root->role = _strdup("desktop");
    root->label = _strdup("Desktop");

    /* Walk top-level children (application windows) */
    IUIAutomationElement *child = nullptr;
    hr = st->walker->GetFirstChildElement(rootElem, &child);
    rootElem->Release();

    if (SUCCEEDED(hr) && child) {
        MdA11yNode *children[MD_UIA_MAX_CHILDREN];
        int childCount = 0;

        while (child && childCount < MD_UIA_MAX_CHILDREN) {
            MdA11yNode *childNode = walk_element(st, child, 1);
            if (childNode)
                children[childCount++] = childNode;

            IUIAutomationElement *next = nullptr;
            hr = st->walker->GetNextSiblingElement(child, &next);
            child->Release();
            child = (SUCCEEDED(hr)) ? next : nullptr;
        }

        if (childCount > 0) {
            root->children = (MdA11yNode **)calloc((size_t)childCount, sizeof(MdA11yNode *));
            if (root->children) {
                memcpy(root->children, children, (size_t)childCount * sizeof(MdA11yNode *));
                root->child_count = childCount;
            }
        }
    }

    *out_root = root;
    return 0;
}

static int uia_get_diff(MdA11yCtx *ctx, MdA11yDelta **out_deltas,
                        int *out_count) {
    auto *st = (UIAState *)ctx->backend_data;
    if (!st || !out_deltas || !out_count) return -1;

    *out_deltas = nullptr;
    *out_count = 0;

    MdA11yNode *current = nullptr;
    if (uia_get_tree(ctx, &current) != 0 || !current)
        return -1;

    MdA11yNode *prev = st->last_snapshot;

    if (!prev) {
        st->last_snapshot = current;
        return 0;
    }

    FlatEntry *prev_flat = nullptr, *curr_flat = nullptr;
    int prev_count = 0, curr_count = 0;
    int prev_cap = 0, curr_cap = 0;

    flatten_tree(prev, &prev_flat, &prev_count, &prev_cap);
    flatten_tree(current, &curr_flat, &curr_count, &curr_cap);

    int max_deltas = prev_count + curr_count;
    MdA11yDelta *deltas = (MdA11yDelta *)calloc((size_t)max_deltas, sizeof(MdA11yDelta));
    if (!deltas) {
        free(prev_flat);
        free(curr_flat);
        md_a11y_node_free(current);
        return -1;
    }

    int dc = 0;

    for (int i = 0; i < prev_count; i++) {
        if (!find_by_id(curr_flat, curr_count, prev_flat[i].id)) {
            deltas[dc].op = MD_A11Y_OP_REMOVE;
            deltas[dc].node = clone_node_shallow(prev_flat[i].node);
            dc++;
        }
    }

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

    md_a11y_node_free(st->last_snapshot);
    st->last_snapshot = current;

    if (dc == 0) {
        free(deltas);
        *out_deltas = nullptr;
        *out_count = 0;
        return 0;
    }

    *out_deltas = deltas;
    *out_count = dc;
    return 0;
}

static int uia_subscribe_changes(MdA11yCtx *ctx, MdA11yChangeCb cb,
                                 void *userdata) {
    (void)ctx; (void)cb; (void)userdata;
    /* Phase 2: IUIAutomationEventHandler for StructureChanged,
     * PropertyChanged (UIA_NamePropertyId, UIA_BoundingRectanglePropertyId). */
    return -1;
}

static void uia_destroy(MdA11yCtx *ctx) {
    auto *st = (UIAState *)ctx->backend_data;
    if (!st) return;

    md_a11y_node_free(st->last_snapshot);

    if (st->walker)     st->walker->Release();
    if (st->automation) st->automation->Release();

    free(st);
    ctx->backend_data = nullptr;

    CoUninitialize();
}

/* ── Singleton vtable ────────────────────────────────────────── */

static const MdA11yBackend uia_backend = {
    uia_init,
    uia_get_tree,
    uia_get_diff,
    uia_subscribe_changes,
    uia_destroy,
};

extern "C"
const MdA11yBackend *md_a11y_backend_create(void) {
    return &uia_backend;
}

#else /* !_WIN32 */

extern "C"
const MdA11yBackend *md_a11y_backend_create(void) {
    fprintf(stderr, "a11y: UI Automation backend not available on this platform\n");
    return nullptr;
}

#endif /* _WIN32 */
