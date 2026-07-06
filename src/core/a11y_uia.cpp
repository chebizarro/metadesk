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
#include <new>

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)
#endif

/* Maximum tree depth */
#define MD_UIA_MAX_DEPTH     32
#define MD_UIA_MAX_CHILDREN  256

/* ── Backend-private state ───────────────────────────────────── */

struct UIAState {
    IUIAutomation      *automation;
    IUIAutomationTreeWalker *walker;
    int                 connected;
    int                 com_initialized;
    uint64_t            next_id;
    MdA11yNode         *last_snapshot;
    CRITICAL_SECTION    lock;
    int                 lock_initialized;

    HANDLE              event_thread;
    HANDLE              stop_event;
    HANDLE              ready_event;
    HRESULT             subscribe_hr;
    int                 subscribed;
    MdA11yChangeCb      change_cb;
    void               *change_userdata;
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

static MdA11yNode *walk_element(UIAState *st, IUIAutomationTreeWalker *walker,
                                 IUIAutomationElement *elem, int depth) {
    if (!st || !walker || !elem || depth > MD_UIA_MAX_DEPTH)
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
    HRESULT hr = walker->GetFirstChildElement(elem, &child);
    if (SUCCEEDED(hr) && child) {
        /* Count and collect children */
        MdA11yNode *children[MD_UIA_MAX_CHILDREN];
        int childCount = 0;

        while (child && childCount < MD_UIA_MAX_CHILDREN) {
            MdA11yNode *childNode = walk_element(st, walker, child, depth + 1);
            if (childNode)
                children[childCount++] = childNode;

            IUIAutomationElement *next = nullptr;
            hr = walker->GetNextSiblingElement(child, &next);
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
    int com_initialized = 0;
    if (SUCCEEDED(hr)) {
        com_initialized = 1;
    } else if (hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "a11y_uia: CoInitializeEx failed: 0x%08lx\n", hr);
        return -1;
    }

    auto *st = (UIAState *)calloc(1, sizeof(UIAState));
    if (!st) {
        if (com_initialized) CoUninitialize();
        return -1;
    }
    st->com_initialized = com_initialized;
    InitializeCriticalSection(&st->lock);
    st->lock_initialized = 1;

    hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                          CLSCTX_INPROC_SERVER,
                          __uuidof(IUIAutomation),
                          (void **)&st->automation);
    if (FAILED(hr) || !st->automation) {
        fprintf(stderr, "a11y_uia: failed to create IUIAutomation: 0x%08lx\n", hr);
        if (st->lock_initialized) DeleteCriticalSection(&st->lock);
        if (st->com_initialized) CoUninitialize();
        free(st);
        return -1;
    }

    /* Create a content tree walker (skips raw/control elements) */
    hr = st->automation->get_ContentViewWalker(&st->walker);
    if (FAILED(hr) || !st->walker) {
        fprintf(stderr, "a11y_uia: failed to get ContentViewWalker\n");
        SAFE_RELEASE(st->automation);
        if (st->lock_initialized) DeleteCriticalSection(&st->lock);
        if (st->com_initialized) CoUninitialize();
        free(st);
        return -1;
    }

    st->connected = 1;
    ctx->backend_data = st;
    return 0;
}

static int uia_get_tree_with_unlocked(UIAState *st, IUIAutomation *automation,
                                      IUIAutomationTreeWalker *walker,
                                      MdA11yNode **out_root) {
    if (!st || !automation || !walker || !st->connected || !out_root)
        return -1;

    *out_root = nullptr;
    st->next_id = 0;

    /* Get the root element (desktop) */
    IUIAutomationElement *rootElem = nullptr;
    HRESULT hr = automation->GetRootElement(&rootElem);
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
    hr = walker->GetFirstChildElement(rootElem, &child);
    rootElem->Release();

    if (SUCCEEDED(hr) && child) {
        MdA11yNode *children[MD_UIA_MAX_CHILDREN];
        int childCount = 0;

        while (child && childCount < MD_UIA_MAX_CHILDREN) {
            MdA11yNode *childNode = walk_element(st, walker, child, 1);
            if (childNode)
                children[childCount++] = childNode;

            IUIAutomationElement *next = nullptr;
            hr = walker->GetNextSiblingElement(child, &next);
            child->Release();
            child = (SUCCEEDED(hr)) ? next : nullptr;
        }

        if (childCount > 0) {
            root->children = (MdA11yNode **)calloc((size_t)childCount, sizeof(MdA11yNode *));
            if (root->children) {
                memcpy(root->children, children, (size_t)childCount * sizeof(MdA11yNode *));
                root->child_count = childCount;
            } else {
                for (int i = 0; i < childCount; i++)
                    md_a11y_node_free(children[i]);
            }
        }
    }

    *out_root = root;
    return 0;
}

static int uia_get_tree(MdA11yCtx *ctx, MdA11yNode **out_root) {
    auto *st = ctx ? (UIAState *)ctx->backend_data : nullptr;
    if (!st || !st->lock_initialized) return -1;

    EnterCriticalSection(&st->lock);
    int ret = uia_get_tree_with_unlocked(st, st->automation, st->walker, out_root);
    LeaveCriticalSection(&st->lock);
    return ret;
}

static int uia_get_diff_with_unlocked(UIAState *st, IUIAutomation *automation,
                                      IUIAutomationTreeWalker *walker,
                                      MdA11yDelta **out_deltas,
                                      int *out_count) {
    if (!st || !automation || !walker || !out_deltas || !out_count)
        return -1;

    *out_deltas = nullptr;
    *out_count = 0;

    MdA11yNode *current = nullptr;
    if (uia_get_tree_with_unlocked(st, automation, walker, &current) != 0 || !current)
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

static int uia_get_diff(MdA11yCtx *ctx, MdA11yDelta **out_deltas,
                        int *out_count) {
    auto *st = ctx ? (UIAState *)ctx->backend_data : nullptr;
    if (!st || !st->lock_initialized) return -1;

    EnterCriticalSection(&st->lock);
    int ret = uia_get_diff_with_unlocked(st, st->automation, st->walker,
                                         out_deltas, out_count);
    LeaveCriticalSection(&st->lock);
    return ret;
}

class UIAChangeHandler final : public IUIAutomationFocusChangedEventHandler,
                               public IUIAutomationStructureChangedEventHandler,
                               public IUIAutomationEventHandler {
public:
    UIAChangeHandler(UIAState *st, IUIAutomation *automation,
                     IUIAutomationTreeWalker *walker)
        : refs_(1), st_(st), automation_(automation), walker_(walker) {
        if (automation_) automation_->AddRef();
        if (walker_) walker_->AddRef();
    }

    virtual ~UIAChangeHandler() {
        SAFE_RELEASE(walker_);
        SAFE_RELEASE(automation_);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;

        if (riid == IID_IUnknown ||
            riid == __uuidof(IUIAutomationFocusChangedEventHandler)) {
            *ppvObject = static_cast<IUIAutomationFocusChangedEventHandler *>(this);
        } else if (riid == __uuidof(IUIAutomationStructureChangedEventHandler)) {
            *ppvObject = static_cast<IUIAutomationStructureChangedEventHandler *>(this);
        } else if (riid == __uuidof(IUIAutomationEventHandler)) {
            *ppvObject = static_cast<IUIAutomationEventHandler *>(this);
        } else {
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return (ULONG)InterlockedIncrement(&refs_);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return (ULONG)refs;
    }

    HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement *sender) override {
        (void)sender;
        emit_change();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE HandleStructureChangedEvent(
        IUIAutomationElement *sender, StructureChangeType changeType,
        SAFEARRAY *runtimeId) override {
        (void)sender;
        (void)changeType;
        (void)runtimeId;
        emit_change();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE HandleAutomationEvent(IUIAutomationElement *sender,
                                                    EVENTID eventId) override {
        (void)sender;
        (void)eventId;
        emit_change();
        return S_OK;
    }

private:
    void emit_change() {
        if (!st_ || !automation_ || !walker_ || !st_->lock_initialized)
            return;

        MdA11yChangeCb cb = nullptr;
        void *userdata = nullptr;
        MdA11yDelta *deltas = nullptr;
        int delta_count = 0;

        EnterCriticalSection(&st_->lock);
        if (st_->subscribed && st_->change_cb) {
            cb = st_->change_cb;
            userdata = st_->change_userdata;
            (void)uia_get_diff_with_unlocked(st_, automation_, walker_,
                                             &deltas, &delta_count);
        }
        LeaveCriticalSection(&st_->lock);

        if (cb)
            cb(deltas, delta_count, userdata);
        md_a11y_delta_free(deltas, delta_count);
    }

    volatile LONG refs_;
    UIAState *st_;
    IUIAutomation *automation_;
    IUIAutomationTreeWalker *walker_;
};

static DWORD WINAPI uia_event_thread_proc(LPVOID data) {
    UIAState *st = (UIAState *)data;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int com_initialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        EnterCriticalSection(&st->lock);
        st->subscribe_hr = hr;
        LeaveCriticalSection(&st->lock);
        SetEvent(st->ready_event);
        return 1;
    }

    IUIAutomation *automation = nullptr;
    IUIAutomationTreeWalker *walker = nullptr;
    IUIAutomationElement *root = nullptr;
    UIAChangeHandler *handler = nullptr;
    bool focus_registered = false;
    bool structure_registered = false;

    hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                          CLSCTX_INPROC_SERVER,
                          __uuidof(IUIAutomation),
                          (void **)&automation);
    if (SUCCEEDED(hr) && automation)
        hr = automation->get_ContentViewWalker(&walker);
    if (SUCCEEDED(hr) && automation)
        hr = automation->GetRootElement(&root);
    if (SUCCEEDED(hr) && !root)
        hr = E_FAIL;
    if (SUCCEEDED(hr) && automation && walker)
        handler = new (std::nothrow) UIAChangeHandler(st, automation, walker);
    if (SUCCEEDED(hr) && !handler)
        hr = E_OUTOFMEMORY;

    if (SUCCEEDED(hr)) {
        /* Seed the baseline before the first pushed event so callbacks emit
         * computed deltas instead of consuming the initial snapshot. */
        EnterCriticalSection(&st->lock);
        if (!st->last_snapshot) {
            MdA11yNode *initial = nullptr;
            if (uia_get_tree_with_unlocked(st, automation, walker, &initial) == 0)
                st->last_snapshot = initial;
        }
        LeaveCriticalSection(&st->lock);

        auto *focus_handler =
            static_cast<IUIAutomationFocusChangedEventHandler *>(handler);
        auto *structure_handler =
            static_cast<IUIAutomationStructureChangedEventHandler *>(handler);

        hr = automation->AddFocusChangedEventHandler(nullptr, focus_handler);
        if (SUCCEEDED(hr)) {
            focus_registered = true;
            hr = automation->AddStructureChangedEventHandler(
                root, TreeScope_Subtree, nullptr, structure_handler);
            if (SUCCEEDED(hr))
                structure_registered = true;
        }
    }

    EnterCriticalSection(&st->lock);
    st->subscribe_hr = hr;
    if (SUCCEEDED(hr))
        st->subscribed = 1;
    LeaveCriticalSection(&st->lock);
    SetEvent(st->ready_event);

    if (SUCCEEDED(hr)) {
        HANDLE stop_event = st->stop_event;
        for (;;) {
            DWORD wait = MsgWaitForMultipleObjects(1, &stop_event, FALSE,
                                                   INFINITE, QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0)
                break;
            if (wait == WAIT_OBJECT_0 + 1) {
                MSG msg;
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }
        }
    }

    EnterCriticalSection(&st->lock);
    st->subscribed = 0;
    LeaveCriticalSection(&st->lock);

    if (structure_registered) {
        auto *structure_handler =
            static_cast<IUIAutomationStructureChangedEventHandler *>(handler);
        automation->RemoveStructureChangedEventHandler(root, structure_handler);
    }
    if (focus_registered) {
        auto *focus_handler =
            static_cast<IUIAutomationFocusChangedEventHandler *>(handler);
        automation->RemoveFocusChangedEventHandler(focus_handler);
    }

    if (handler) handler->Release();
    SAFE_RELEASE(root);
    SAFE_RELEASE(walker);
    SAFE_RELEASE(automation);
    if (com_initialized)
        CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}

static int uia_subscribe_changes(MdA11yCtx *ctx, MdA11yChangeCb cb,
                                 void *userdata) {
    auto *st = ctx ? (UIAState *)ctx->backend_data : nullptr;
    if (!st || !st->connected || !st->lock_initialized || !cb)
        return -1;

    EnterCriticalSection(&st->lock);
    if (st->subscribed) {
        st->change_cb = cb;
        st->change_userdata = userdata;
        LeaveCriticalSection(&st->lock);
        return 0;
    }
    if (st->event_thread) {
        LeaveCriticalSection(&st->lock);
        return -1;
    }
    st->change_cb = cb;
    st->change_userdata = userdata;
    st->subscribe_hr = E_PENDING;
    LeaveCriticalSection(&st->lock);

    st->stop_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    st->ready_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!st->stop_event || !st->ready_event) {
        if (st->stop_event) CloseHandle(st->stop_event);
        if (st->ready_event) CloseHandle(st->ready_event);
        st->stop_event = nullptr;
        st->ready_event = nullptr;
        EnterCriticalSection(&st->lock);
        st->change_cb = nullptr;
        st->change_userdata = nullptr;
        LeaveCriticalSection(&st->lock);
        return -1;
    }

    st->event_thread = CreateThread(nullptr, 0, uia_event_thread_proc,
                                    st, 0, nullptr);
    if (!st->event_thread) {
        CloseHandle(st->stop_event);
        CloseHandle(st->ready_event);
        st->stop_event = nullptr;
        st->ready_event = nullptr;
        EnterCriticalSection(&st->lock);
        st->change_cb = nullptr;
        st->change_userdata = nullptr;
        LeaveCriticalSection(&st->lock);
        return -1;
    }

    WaitForSingleObject(st->ready_event, INFINITE);

    HRESULT hr;
    EnterCriticalSection(&st->lock);
    hr = st->subscribe_hr;
    LeaveCriticalSection(&st->lock);

    if (FAILED(hr)) {
        WaitForSingleObject(st->event_thread, INFINITE);
        CloseHandle(st->event_thread);
        CloseHandle(st->stop_event);
        CloseHandle(st->ready_event);
        st->event_thread = nullptr;
        st->stop_event = nullptr;
        st->ready_event = nullptr;
        EnterCriticalSection(&st->lock);
        st->change_cb = nullptr;
        st->change_userdata = nullptr;
        LeaveCriticalSection(&st->lock);
        fprintf(stderr, "a11y_uia: failed to subscribe UIA events: 0x%08lx\n", hr);
        return -1;
    }

    return 0;
}

static void uia_destroy(MdA11yCtx *ctx) {
    auto *st = ctx ? (UIAState *)ctx->backend_data : nullptr;
    if (!st) return;

    if (st->lock_initialized) {
        EnterCriticalSection(&st->lock);
        st->connected = 0;
        st->subscribed = 0;
        st->change_cb = nullptr;
        st->change_userdata = nullptr;
        LeaveCriticalSection(&st->lock);
    }

    if (st->event_thread) {
        if (st->stop_event)
            SetEvent(st->stop_event);
        WaitForSingleObject(st->event_thread, INFINITE);
        CloseHandle(st->event_thread);
        st->event_thread = nullptr;
    }
    if (st->stop_event) {
        CloseHandle(st->stop_event);
        st->stop_event = nullptr;
    }
    if (st->ready_event) {
        CloseHandle(st->ready_event);
        st->ready_event = nullptr;
    }

    md_a11y_node_free(st->last_snapshot);

    SAFE_RELEASE(st->walker);
    SAFE_RELEASE(st->automation);

    if (st->lock_initialized)
        DeleteCriticalSection(&st->lock);

    if (st->com_initialized)
        CoUninitialize();

    free(st);
    ctx->backend_data = nullptr;
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
