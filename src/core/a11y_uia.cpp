/*
 * metadesk — a11y_uia.cpp
 * Windows accessibility backend: UI Automation (spec §2.3.2).
 *
 * Uses Microsoft UI Automation COM API to:
 *   1. CoCreateInstance IUIAutomation
 *   2. GetRootElement → walk tree via TreeWalker, prefetching each top-level
 *      window's subtree with one IUIAutomationCacheRequest round trip
 *   3. Extract (from the cache): ControlType, Name, BoundingRectangle, etc.
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
#include <commctrl.h>   /* item-count messages only; no comctl32 link */
#include <wtsapi32.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <new>
#include "log.h"
#define MD_LOG_TAG "a11y_uia"

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)
#endif

/* Maximum tree depth */
#define MD_UIA_MAX_DEPTH     32
#define MD_UIA_MAX_CHILDREN  256

/* A Win32 item container holding more than this many items makes its
 * top-level window walk by bounded navigation instead of one subtree fetch
 * (see "Tree walking" below). At 4x the child cap, the overfetch a subtree
 * fetch would pay (~0.3 ms/item through the in-process Win32 proxies) is
 * about what bounded navigation (~1.5 ms/node) costs for a typical window. */
#define MD_UIA_WIDE_ITEMS        (4 * MD_UIA_MAX_CHILDREN)
/* Per-control bound on the item-count query to a foreign window. */
#define MD_UIA_WIDE_PROBE_MS     100
/* Wide windows remembered per walk; any beyond this use the subtree fetch. */
#define MD_UIA_MAX_WIDE_WINDOWS  16

/* ── Backend-private state ───────────────────────────────────── */

struct UIAState {
    IUIAutomation      *automation;
    IUIAutomationTreeWalker *walker;
    MdA11yCtx          *ctx;         /* back-pointer for change hints */
    volatile LONG       connected;  /* Interlocked only; predicate never calls COM */
    int                 com_initialized;
    uint64_t            next_id;
    CRITICAL_SECTION    lock;
    int                 lock_initialized;
    SRWLOCK             calls_lock;
    CONDITION_VARIABLE  calls_drained;
    unsigned            active_calls; /* includes callers waiting for tree lock */
    bool                closing;

    /* Resolve WTS at runtime to keep the backend self-contained in existing
     * Windows link targets (which do not link wtsapi32). Fail closed if absent. */
    HMODULE             wts_module;
    decltype(&WTSQuerySessionInformationW) wts_query;
    decltype(&WTSFreeMemory) wts_free;
    decltype(&WTSRegisterSessionNotification) wts_register;
    decltype(&WTSUnRegisterSessionNotification) wts_unregister;
    DWORD               session_id;
    HANDLE              session_thread;
    HANDLE              session_stop;
    HANDLE              session_ready;
    HWND                session_window;
    int                 session_registered;

    HANDLE              event_thread;
    HANDLE              stop_event;
    HANDLE              ready_event;
    HRESULT             subscribe_hr;
    int                 subscribed;      /* handlers registered; the callback
                                          * itself lives on the MdA11yCtx */
};

/* Loss is terminal for this context: unlocking/reconnecting requires a new
 * backend, not resurrection of COM proxies from the old desktop. */
static bool uia_connected(UIAState *st) {
    return st && InterlockedCompareExchange(&st->connected, 0, 0) != 0;
}

#ifdef MD_UIA_TESTING
static HANDLE uia_test_loss_event = nullptr;
#endif

static void uia_lost(UIAState *st) {
    InterlockedExchange(&st->connected, 0);
#ifdef MD_UIA_TESTING
    if (uia_test_loss_event) SetEvent(uia_test_loss_event);
#endif
}

static void uia_session_change(UIAState *st, WPARAM reason, DWORD session) {
    if (session == st->session_id &&
        (reason == WTS_SESSION_LOCK || reason == WTS_CONSOLE_DISCONNECT ||
         reason == WTS_REMOTE_DISCONNECT || reason == WTS_SESSION_LOGOFF ||
         reason == WTS_SESSION_TERMINATE))
        uia_lost(st);
}

static LRESULT CALLBACK uia_session_proc(HWND window, UINT msg,
                                         WPARAM wp, LPARAM lp) {
    auto *st = reinterpret_cast<UIAState *>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        st = static_cast<UIAState *>(reinterpret_cast<CREATESTRUCT *>(lp)->lpCreateParams);
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
    }
    if (st && msg == WM_WTSSESSION_CHANGE)
        uia_session_change(st, wp, static_cast<DWORD>(lp));
    if (st && msg == WM_ENDSESSION && wp)
        uia_lost(st);
    return DefWindowProc(window, msg, wp, lp);
}

/* Session 0 can allocate UIA and even obtain a root, but has no interactive
 * desktop. Register before checking state so startup cannot miss a loss.
 * OpenInputDesktop also rejects a locked/inaccessible secure desktop; compare
 * names to avoid claiming readiness on a different desktop. No COM here. */
static bool uia_desktop_available(UIAState *st) {
    if (!st->session_id) return false;
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    bool active = st->wts_query(WTS_CURRENT_SERVER_HANDLE,
        st->session_id, WTSConnectState, &buffer, &bytes) &&
        bytes >= sizeof(WTS_CONNECTSTATE_CLASS) &&
        *reinterpret_cast<WTS_CONNECTSTATE_CLASS *>(buffer) == WTSActive;
    if (buffer) st->wts_free(buffer);
    if (!active) return false;
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) return false;
    wchar_t input_name[256], thread_name[256];
    DWORD needed = 0;
    bool same = GetUserObjectInformationW(input, UOI_NAME, input_name,
                    sizeof(input_name), &needed) &&
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),
                    UOI_NAME, thread_name, sizeof(thread_name), &needed) &&
        wcscmp(input_name, thread_name) == 0;
    CloseDesktop(input);
    return same;
}

static DWORD WINAPI uia_session_thread_proc(LPVOID data) {
    auto *st = static_cast<UIAState *>(data);
    /* A per-thread class name allows independent backend contexts. This thread
     * owns a hidden top-level window, never COM/UIA (UIA calls must not run on
     * a window-owning thread). */
    wchar_t class_name[64];
    swprintf(class_name, 64, L"MetadeskUIASession%lu", GetCurrentThreadId());
    WNDCLASSW wc = {};
    wc.lpfnWndProc = uia_session_proc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = class_name;
    ATOM atom = RegisterClassW(&wc);
    HWND window = atom ? CreateWindowExW(0, class_name, L"", 0,
        0, 0, 0, 0, nullptr, nullptr, wc.hInstance, st) : nullptr;
    bool registered = window &&
        st->wts_register(window, NOTIFY_FOR_THIS_SESSION);
    st->session_window = window;
    st->session_registered = registered;
    if (!registered || !uia_desktop_available(st)) uia_lost(st);
    SetEvent(st->session_ready);
    if (registered) {
        for (;;) {
            DWORD wait = MsgWaitForMultipleObjects(1, &st->session_stop,
                                                   FALSE, INFINITE, QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0) break;
            if (wait != WAIT_OBJECT_0 + 1) { uia_lost(st); break; }
            MSG msg;
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
        st->wts_unregister(window);
    }
    if (window) DestroyWindow(window);
    if (atom) UnregisterClassW(class_name, wc.hInstance);
    return 0;
}

static void uia_stop_session_thread(UIAState *st) {
    if (st->session_thread) {
        SetEvent(st->session_stop);
        WaitForSingleObject(st->session_thread, INFINITE);
        CloseHandle(st->session_thread);
    }
    if (st->session_stop) CloseHandle(st->session_stop);
    if (st->session_ready) CloseHandle(st->session_ready);
    if (st->wts_module) FreeLibrary(st->wts_module);
}

#ifdef MD_UIA_TESTING
/* Only the standalone harness defines this seam; never present in production. */
static HRESULT (*uia_test_root)(IUIAutomation *, IUIAutomationElement **) = nullptr;
static HANDLE uia_test_destroy_waiting = nullptr;
static HANDLE uia_test_call_admitted = nullptr;
#endif

static HRESULT uia_root(UIAState *st, IUIAutomation *automation,
                         IUIAutomationElement **root) {
    HRESULT hr;
#ifdef MD_UIA_TESTING
    if (uia_test_root) hr = uia_test_root(automation, root);
    else
#endif
        hr = automation->GetRootElement(root);
    if (SUCCEEDED(hr) && *root) {
        int process_id = 0;
        hr = (*root)->get_CurrentProcessId(&process_id);
    }
    /* Only a desktop-root failure is global. A child disappearing during a
     * walk is normal and must not terminate the host. */
    if (hr == RPC_E_DISCONNECTED || hr == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) ||
        hr == CO_E_OBJNOTCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE))
        uia_lost(st);
    if (SUCCEEDED(hr) && !*root) { uia_lost(st); hr = E_FAIL; }
    if (FAILED(hr)) SAFE_RELEASE(*root);
    return hr;
}

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

#define MD_UIA_ROLES(X) \
    X(UIA_ButtonControlTypeId, MD_A11Y_ROLE_BUTTON) \
    X(UIA_CheckBoxControlTypeId, MD_A11Y_ROLE_CHECK_BOX) \
    X(UIA_RadioButtonControlTypeId, MD_A11Y_ROLE_RADIO_BUTTON) \
    X(UIA_ComboBoxControlTypeId, MD_A11Y_ROLE_COMBO_BOX) \
    X(UIA_EditControlTypeId, MD_A11Y_ROLE_TEXT) \
    X(UIA_ListControlTypeId, MD_A11Y_ROLE_LIST) \
    X(UIA_ListItemControlTypeId, MD_A11Y_ROLE_LIST_ITEM) \
    X(UIA_MenuControlTypeId, MD_A11Y_ROLE_MENU) \
    X(UIA_MenuBarControlTypeId, MD_A11Y_ROLE_MENU) \
    X(UIA_MenuItemControlTypeId, MD_A11Y_ROLE_MENU_ITEM) \
    X(UIA_TabControlTypeId, MD_A11Y_ROLE_PAGE_TAB_LIST) \
    X(UIA_TabItemControlTypeId, MD_A11Y_ROLE_TAB) \
    X(UIA_TreeControlTypeId, MD_A11Y_ROLE_TREE) \
    X(UIA_TreeItemControlTypeId, MD_A11Y_ROLE_TREE_ITEM) \
    X(UIA_SliderControlTypeId, MD_A11Y_ROLE_SLIDER) \
    X(UIA_ScrollBarControlTypeId, MD_A11Y_ROLE_SCROLL_BAR) \
    X(UIA_ProgressBarControlTypeId, MD_A11Y_ROLE_PROGRESS_BAR) \
    X(UIA_StatusBarControlTypeId, MD_A11Y_ROLE_STATUS_BAR) \
    X(UIA_ToolBarControlTypeId, MD_A11Y_ROLE_TOOL_BAR) \
    X(UIA_WindowControlTypeId, MD_A11Y_ROLE_WINDOW) \
    X(UIA_PaneControlTypeId, MD_A11Y_ROLE_PANEL) \
    X(UIA_GroupControlTypeId, MD_A11Y_ROLE_PANEL) \
    X(UIA_TextControlTypeId, MD_A11Y_ROLE_LABEL) \
    X(UIA_ImageControlTypeId, MD_A11Y_ROLE_IMAGE) \
    X(UIA_HyperlinkControlTypeId, MD_A11Y_ROLE_LINK) \
    X(UIA_DocumentControlTypeId, MD_A11Y_ROLE_DOCUMENT) \
    X(UIA_TableControlTypeId, MD_A11Y_ROLE_TABLE) \
    /* Header is the container of HeaderItems (a column-header row), not a
     * heading; UIA expresses headings via the HeadingLevel property. */ \
    X(UIA_HeaderControlTypeId, MD_A11Y_ROLE_PANEL) \
    X(UIA_HeaderItemControlTypeId, MD_A11Y_ROLE_COLUMN_HEADER) \
    X(UIA_DataGridControlTypeId, MD_A11Y_ROLE_TABLE) \
    X(UIA_DataItemControlTypeId, MD_A11Y_ROLE_TABLE_CELL) \
    X(UIA_SplitButtonControlTypeId, MD_A11Y_ROLE_BUTTON) \
    X(UIA_SpinnerControlTypeId, MD_A11Y_ROLE_SPIN_BUTTON) \
    X(UIA_ToolTipControlTypeId, MD_A11Y_ROLE_TOOL_TIP) \
    X(UIA_SeparatorControlTypeId, MD_A11Y_ROLE_SEPARATOR) \
    /* Matches the AT-SPI "title bar" alias in a11y_vocab.h. */ \
    X(UIA_TitleBarControlTypeId, MD_A11Y_ROLE_LABEL) \
    X(UIA_CalendarControlTypeId, MD_A11Y_ROLE_CALENDAR) \
    X(UIA_AppBarControlTypeId, MD_A11Y_ROLE_TOOL_BAR) \
    X(UIA_SemanticZoomControlTypeId, MD_A11Y_ROLE_LIST) \
    /* Deliberately UNKNOWN: Thumb is a scroll-bar drag handle and Custom has
     * no fixed semantics. Listed so they are not logged as unmapped. */ \
    X(UIA_ThumbControlTypeId, MD_A11Y_ROLE_UNKNOWN) \
    X(UIA_CustomControlTypeId, MD_A11Y_ROLE_UNKNOWN)

#define MD_UIA_ASSERT(native, role) static_assert(role >= 0 && role < MD_A11Y_ROLE_COUNT, "invalid UIA role");
MD_UIA_ROLES(MD_UIA_ASSERT)
#undef MD_UIA_ASSERT
static const struct { CONTROLTYPEID native; enum MdA11yRole role; } uia_roles[] = {
#define MD_UIA_ROW(native, role) { native, role },
    MD_UIA_ROLES(MD_UIA_ROW)
#undef MD_UIA_ROW
};
#undef MD_UIA_ROLES
/* Log each unmapped control type once (never per node per walk). UIA control
 * types are a fixed set (UIA_*ControlTypeId, 50000..50040), so a 128-bit
 * bitmap indexed from 50000 covers them; an out-of-range id (not produced by
 * UIA) would be logged on every occurrence. */
static void note_unmapped_control_type(CONTROLTYPEID ctid) {
    static volatile LONG seen[4] = {0, 0, 0, 0}; /* 128 bits from 50000 */
    long off = (long)ctid - 50000;
    if (off >= 0 && off < 128) {
        LONG bit = (LONG)1 << (off % 32);
        LONG prev = InterlockedOr(&seen[off / 32], bit);
        if (prev & bit) return;
    }
    MD_LOG_I("unmapped UIA control type %d -> unknown", (int)ctid);
}

static const char *control_type_to_role(CONTROLTYPEID ctid) {
    for (const auto &entry : uia_roles)
        if (entry.native == ctid) return md_a11y_role_name(entry.role);
    note_unmapped_control_type(ctid);
    return md_a11y_role_name(MD_A11Y_ROLE_UNKNOWN);
}

/* Extract states from cached UIA element properties */
static void extract_states(IUIAutomationElement *elem, MdA11yNode *node) {
    char *states[8];
    int count = 0;

    BOOL boolVal;

    if (SUCCEEDED(elem->get_CachedIsEnabled(&boolVal)) && boolVal)
        states[count++] = _strdup(md_a11y_state_name(MD_A11Y_STATE_ENABLED));

    if (SUCCEEDED(elem->get_CachedHasKeyboardFocus(&boolVal)) && boolVal)
        states[count++] = _strdup(md_a11y_state_name(MD_A11Y_STATE_FOCUSED));

    if (SUCCEEDED(elem->get_CachedIsKeyboardFocusable(&boolVal)) && boolVal)
        states[count++] = _strdup(md_a11y_state_name(MD_A11Y_STATE_FOCUSABLE));

    if (SUCCEEDED(elem->get_CachedIsOffscreen(&boolVal)) && !boolVal) {
        states[count++] = _strdup(md_a11y_state_name(MD_A11Y_STATE_VISIBLE));
        states[count++] = _strdup(md_a11y_state_name(MD_A11Y_STATE_SHOWING));
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

/* ── Tree walking ────────────────────────────────────────────── *
 *
 * Every Current* property read and TreeWalker step is a cross-process round
 * trip; reading eight properties plus two navigation steps per node cost
 * several ms/node. The walk instead asks UIA to prefetch what it needs:
 * top-level windows are still enumerated with the ContentViewWalker, but
 * through the *BuildCache navigation calls with a TreeScope_Subtree request,
 * so each window arrives with its whole content-view subtree and every walked
 * property in one round trip. Nodes are then built from get_Cached* reads and
 * GetCachedChildren, which stay in-process. The request's TreeFilter is the
 * content-view condition, i.e. the same view ContentViewWalker navigates.
 *
 * If a window's subtree fetch fails, navigation is retried with an
 * element-only request and the window is walked one Element|Children fetch
 * per node, so one bad app degrades to per-level cost instead of vanishing or
 * ending enumeration.
 *
 * Width bound (md-0by): UIA cannot bound a subtree cache fetch by depth or
 * width (the transaction timeout is per provider call, not per request), so
 * nodes past MD_UIA_MAX_DEPTH / MD_UIA_MAX_CHILDREN are fetched and then
 * dropped. That overfetch is expensive where the provider is one of the
 * in-process Win32 proxies (list box, list view, tree view, combo box): each
 * item costs cross-process window messages, so a 5000-item list box made the
 * cached walk ~2x slower than the capped uncached one. Native providers serve
 * a subtree request inside their own process, where overfetch is cheap.
 *
 * So before walking, uia_scan_wide() checks the visible Win32 descendants of
 * every top-level window for those control classes and asks each for its
 * item count (window messages only, no COM). Top-level windows holding a
 * container with more than MD_UIA_WIDE_ITEMS items are walked by bounded
 * navigation instead: First/NextSiblingElementBuildCache with an element
 * request, never past either cap, which is exactly the uncached walk's
 * semantics at two round trips per node. When some window is wide, every
 * top-level window is navigated with the element request first (to read its
 * HWND) and then, if not wide, refreshed with the subtree request: one more
 * round trip per window, paid only while a wide list is on screen. When none
 * is, enumeration is the single subtree-fetching navigation as before. */

/* Everything walk_node() and extract_states() read. Element-only requests
 * also carry UIA_NativeWindowHandlePropertyId for the wide-window check. */
static const PROPERTYID uia_walk_props[] = {
    UIA_RuntimeIdPropertyId,
    UIA_ControlTypePropertyId,
    UIA_NamePropertyId,
    UIA_IsEnabledPropertyId,
    UIA_HasKeyboardFocusPropertyId,
    UIA_IsKeyboardFocusablePropertyId,
    UIA_IsOffscreenPropertyId,
    UIA_BoundingRectanglePropertyId,
};

struct UIAWalk {
    IUIAutomationTreeWalker   *walker;   /* borrowed: ContentViewWalker */
    IUIAutomationCacheRequest *subtree;  /* props + Subtree */
    IUIAutomationCacheRequest *level;    /* props + Element|Children */
    IUIAutomationCacheRequest *element;  /* props + HWND + Element */
    HWND wide[MD_UIA_MAX_WIDE_WINDOWS];  /* top-level windows to bound */
    int  wide_count;
};

/* What a cached element carries beyond its own properties. */
enum UIACached {
    UIA_CACHED_ELEMENT,   /* no children cached */
    UIA_CACHED_CHILDREN,  /* its children (with properties) are cached */
    UIA_CACHED_SUBTREE,   /* every descendant's children are cached too */
    UIA_CACHED_BOUNDED,   /* no children cached: navigate to them, each
                           * arriving with its own properties */
};

static void uia_walk_end(UIAWalk *w) {
    SAFE_RELEASE(w->subtree);
    SAFE_RELEASE(w->level);
    SAFE_RELEASE(w->element);
}

static HRESULT uia_cache_request(IUIAutomation *automation,
                                 IUIAutomationCondition *view, TreeScope scope,
                                 PROPERTYID extra,
                                 IUIAutomationCacheRequest **out) {
    IUIAutomationCacheRequest *req = nullptr;
    HRESULT hr = automation->CreateCacheRequest(&req);
    if (SUCCEEDED(hr) && !req) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = req->put_TreeFilter(view);
    if (SUCCEEDED(hr)) hr = req->put_TreeScope(scope);
    for (PROPERTYID id : uia_walk_props)
        if (SUCCEEDED(hr)) hr = req->AddProperty(id);
    if (SUCCEEDED(hr) && extra) hr = req->AddProperty(extra);
    if (FAILED(hr)) SAFE_RELEASE(req);
    *out = req;
    return hr;
}

/* Cache requests are client-side objects: building them makes no
 * cross-process call. */
static HRESULT uia_walk_begin(IUIAutomation *automation,
                              IUIAutomationTreeWalker *walker, UIAWalk *w) {
    *w = {};
    w->walker = walker;
    IUIAutomationCondition *view = nullptr;
    HRESULT hr = automation->get_ContentViewCondition(&view);
    if (SUCCEEDED(hr) && !view) hr = E_FAIL;
    if (SUCCEEDED(hr))
        hr = uia_cache_request(automation, view, TreeScope_Subtree, 0, &w->subtree);
    if (SUCCEEDED(hr))
        hr = uia_cache_request(automation, view,
            static_cast<TreeScope>(TreeScope_Element | TreeScope_Children), 0, &w->level);
    if (SUCCEEDED(hr))
        hr = uia_cache_request(automation, view, TreeScope_Element,
                               UIA_NativeWindowHandlePropertyId, &w->element);
    SAFE_RELEASE(view);
    if (FAILED(hr)) uia_walk_end(w);
    return hr;
}

/* ── Wide Win32 item containers (md-0by) ─────────────────────── */

#ifdef MD_UIA_TESTING
/* Only the standalone harness defines this seam: a nonzero value replaces
 * MD_UIA_WIDE_ITEMS so the default fixture exercises the bounded path. */
static LRESULT uia_test_wide_items = 0;
#endif

static LRESULT uia_wide_items() {
#ifdef MD_UIA_TESTING
    if (uia_test_wide_items) return uia_test_wide_items;
#endif
    return MD_UIA_WIDE_ITEMS;
}

/* The item-count message for a Win32 control whose UIA proxy exposes one
 * element per item, or 0. Matches exact class names (and their WinForms
 * wrappers, "WindowsForms10.<class>.app...") so that no private message is
 * ever sent to an unrelated window class. */
static UINT uia_item_count_msg(HWND h) {
    wchar_t cls[128];
    if (GetClassNameW(h, cls, 128) <= 0) return 0;
    static const wchar_t winforms[] = L"WindowsForms10.";
    const size_t prefix = sizeof(winforms) / sizeof(winforms[0]) - 1;
    const wchar_t *base = cls;
    if (_wcsnicmp(cls, winforms, prefix) == 0) {
        base = cls + prefix;
        if (wchar_t *dot = wcschr(cls + prefix, L'.')) *dot = L'\0';
    }
    if (_wcsicmp(base, L"ListBox") == 0 || _wcsicmp(base, L"ComboLBox") == 0)
        return LB_GETCOUNT;
    if (_wcsicmp(base, L"ComboBox") == 0) return CB_GETCOUNT;
    if (_wcsicmp(base, WC_LISTVIEWW) == 0) return LVM_GETITEMCOUNT;
    if (_wcsicmp(base, WC_TREEVIEWW) == 0) return TVM_GETCOUNT;
    return 0;
}

/* A hung or unreachable (UIPI) control counts as narrow: the walk then
 * behaves as it did before md-0by. */
static bool uia_hwnd_wide(HWND h) {
    UINT msg = IsWindowVisible(h) ? uia_item_count_msg(h) : 0;
    DWORD_PTR count = 0;
    return msg && SendMessageTimeoutW(h, msg, 0, 0, SMTO_ABORTIFHUNG,
                                      MD_UIA_WIDE_PROBE_MS, &count) &&
        static_cast<LRESULT>(count) > uia_wide_items();
}

static BOOL CALLBACK uia_scan_descendant(HWND h, LPARAM data) {
    auto *wide = reinterpret_cast<bool *>(data);
    *wide = uia_hwnd_wide(h);
    return !*wide;
}

/* Whether top-level window `h` is, or contains, a wide item container. */
static bool uia_window_wide(HWND h) {
    bool wide = uia_hwnd_wide(h);
    if (!wide)
        EnumChildWindows(h, uia_scan_descendant, reinterpret_cast<LPARAM>(&wide));
    return wide;
}

struct UIAWideScan {
    UIAWalk *w;
    HWND parent;  /* nullptr: the desktop's top-level windows */
};

static BOOL CALLBACK uia_scan_top(HWND h, LPARAM data) {
    auto *scan = reinterpret_cast<UIAWideScan *>(data);
    /* EnumChildWindows visits all descendants; keep direct children. */
    if (scan->parent && GetAncestor(h, GA_PARENT) != scan->parent) return TRUE;
    if (IsWindowVisible(h) && uia_window_wide(h))
        scan->w->wide[scan->w->wide_count++] = h;
    return scan->w->wide_count < MD_UIA_MAX_WIDE_WINDOWS;
}

/* Fill w->wide with the windows under `parent` (nullptr: the desktop) that
 * must be walked by bounded navigation. In-process window-manager calls plus
 * one bounded message per matching control; no UIA round trip. */
static void uia_scan_wide(UIAWalk *w, HWND parent) {
    w->wide_count = 0;
    UIAWideScan scan = { w, parent };
    if (parent)
        EnumChildWindows(parent, uia_scan_top, reinterpret_cast<LPARAM>(&scan));
    else
        EnumWindows(uia_scan_top, reinterpret_cast<LPARAM>(&scan));
}

static bool uia_is_wide(const UIAWalk *w, IUIAutomationElement *elem) {
    UIA_HWND h = nullptr;
    if (!w->wide_count || FAILED(elem->get_CachedNativeWindowHandle(&h)) || !h)
        return false;
    for (int i = 0; i < w->wide_count; i++)
        if (w->wide[i] == static_cast<HWND>(h)) return true;
    return false;
}

/* Format a UIA RuntimeId (VT_I4 SAFEARRAY) as "u:2a.101b0.4". */
static void set_runtime_id(MdA11yNode *node, SAFEARRAY *rid) {
    LONG lb = 0, ub = -1;
    SafeArrayGetLBound(rid, 1, &lb);
    SafeArrayGetUBound(rid, 1, &ub);
    char buf[256];
    size_t off = 0;
    for (LONG i = lb; i <= ub && off < sizeof(buf) - 12; i++) {
        int v = 0;
        if (SUCCEEDED(SafeArrayGetElement(rid, &i, &v)))
            off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                                    "%s%x", i == lb ? "u:" : ".",
                                    (unsigned)v);
    }
    if (off > 2) {
        free(node->id);
        node->id = _strdup(buf);
    }
}

#ifdef MD_UIA_TESTING
/* Only the standalone harness defines this seam: fetches using these requests
 * fail, forcing walk_top_level_step's fallback tiers. */
static const IUIAutomationCacheRequest *uia_test_rejected[2] = { nullptr, nullptr };
#endif

static bool uia_fetch_rejected(const IUIAutomationCacheRequest *req) {
#ifdef MD_UIA_TESTING
    return req && (req == uia_test_rejected[0] || req == uia_test_rejected[1]);
#else
    (void)req;
    return false;
#endif
}

/* Replace *elem with a copy carrying `req`'s cache. On failure *elem is
 * untouched and false is returned. */
static bool uia_refresh(IUIAutomationCacheRequest *req, IUIAutomationElement **elem) {
    IUIAutomationElement *refreshed = nullptr;
    if (uia_fetch_rejected(req) ||
        FAILED((*elem)->BuildUpdatedCache(req, &refreshed)) || !refreshed) {
        SAFE_RELEASE(refreshed);
        return false;
    }
    (*elem)->Release();
    *elem = refreshed;
    return true;
}

static MdA11yNode *walk_node(UIAState *st, const UIAWalk *w,
                             IUIAutomationElement *elem, int depth,
                             UIACached cached);

/* Trim a child array allocated at the cap to what was emitted. */
static void walk_children_trim(MdA11yNode *node) {
    if (node->child_count == 0) {
        free(node->children);
        node->children = nullptr;
    } else if (node->child_count < MD_UIA_MAX_CHILDREN) {
        auto *exact = (MdA11yNode **)realloc(node->children,
            (size_t)node->child_count * sizeof(MdA11yNode *));
        if (exact) node->children = exact;
    }
}

/* UIA_CACHED_BOUNDED: reach elem's children by navigation, each arriving
 * with its own properties, and never navigate past the child cap. A failed
 * first-child step leaves elem a leaf; a failed sibling step ends the list,
 * as in the uncached walk. Returns false only on allocation failure. */
static bool walk_children_bounded(UIAState *st, const UIAWalk *w,
                                  IUIAutomationElement *elem, int depth,
                                  MdA11yNode *node) {
    IUIAutomationElement *child = nullptr;
    HRESULT hr = w->walker->GetFirstChildElementBuildCache(elem, w->element, &child);
    if (FAILED(hr) || !child) {
        SAFE_RELEASE(child);
        return true;
    }
    node->children = (MdA11yNode **)calloc(MD_UIA_MAX_CHILDREN, sizeof(MdA11yNode *));
    if (!node->children) {
        child->Release();
        return false;
    }
    while (child) {
        MdA11yNode *child_node = walk_node(st, w, child, depth + 1, UIA_CACHED_BOUNDED);
        if (child_node) node->children[node->child_count++] = child_node;
        if (node->child_count == MD_UIA_MAX_CHILDREN) break;
        IUIAutomationElement *next = nullptr;
        hr = w->walker->GetNextSiblingElementBuildCache(child, w->element, &next);
        child->Release();
        child = SUCCEEDED(hr) ? next : nullptr;
    }
    SAFE_RELEASE(child);
    walk_children_trim(node);
    return true;
}

/* Attach elem's cached children. In UIA_CACHED_CHILDREN mode each child is
 * refreshed with the per-level request (one round trip per node) so that its
 * own children are cached; if that refresh fails the child is a leaf, as when
 * the uncached GetFirstChildElement failed. In UIA_CACHED_SUBTREE mode
 * nothing is fetched. Returns false only on allocation failure, which (as
 * before) discards the parent. */
static bool walk_children(UIAState *st, const UIAWalk *w,
                          IUIAutomationElement *elem, int depth,
                          UIACached cached, MdA11yNode *node) {
    IUIAutomationElementArray *kids = nullptr;
    int len = 0;
    if (FAILED(elem->GetCachedChildren(&kids)) || !kids ||
        FAILED(kids->get_Length(&len)) || len <= 0) {
        SAFE_RELEASE(kids);
        return true;
    }
    node->children = (MdA11yNode **)calloc(
        (size_t)(len < MD_UIA_MAX_CHILDREN ? len : MD_UIA_MAX_CHILDREN),
        sizeof(MdA11yNode *));
    if (!node->children) {
        kids->Release();
        return false;
    }
    /* The cap counts emitted children, not attempted ones. */
    for (int i = 0; i < len && node->child_count < MD_UIA_MAX_CHILDREN; i++) {
        IUIAutomationElement *child = nullptr;
        if (FAILED(kids->GetElement(i, &child)) || !child) continue;
        UIACached child_cached = UIA_CACHED_SUBTREE;
        if (cached != UIA_CACHED_SUBTREE) {
            /* Children of a node at the depth cap are never emitted. */
            child_cached = depth + 1 < MD_UIA_MAX_DEPTH && uia_refresh(w->level, &child)
                ? UIA_CACHED_CHILDREN : UIA_CACHED_ELEMENT;
        }
        MdA11yNode *child_node = walk_node(st, w, child, depth + 1, child_cached);
        child->Release();
        if (child_node) node->children[node->child_count++] = child_node;
    }
    if (node->child_count == 0) {
        free(node->children);
        node->children = nullptr;
    }
    kids->Release();
    return true;
}

/* Build a node from elem's cache. Ids are assigned in DFS preorder, as the
 * uncached TreeWalker walk did, so RuntimeId-less nodes keep their ids. */
static MdA11yNode *walk_node(UIAState *st, const UIAWalk *w,
                             IUIAutomationElement *elem, int depth,
                             UIACached cached) {
    if (!st || !w || !elem || depth > MD_UIA_MAX_DEPTH)
        return nullptr;

    MdA11yNode *node = (MdA11yNode *)calloc(1, sizeof(MdA11yNode));
    if (!node) return nullptr;

    node->id = make_node_id(st);

    /* Prefer the platform's stable element identity over DFS position.
     * UIA RuntimeId survives across walks; the DFS counter does not. */
    VARIANT rid;
    VariantInit(&rid);
    if (SUCCEEDED(elem->GetCachedPropertyValue(UIA_RuntimeIdPropertyId, &rid)) &&
        rid.vt == (VT_ARRAY | VT_I4) && rid.parray)
        set_runtime_id(node, rid.parray);
    VariantClear(&rid);

    /* Role (ControlType) */
    CONTROLTYPEID ctid = 0;
    elem->get_CachedControlType(&ctid);
    node->role = _strdup(control_type_to_role(ctid));

    /* Label (Name property) */
    BSTR bstrName = nullptr;
    if (SUCCEEDED(elem->get_CachedName(&bstrName)) && bstrName) {
        node->label = bstr_to_cstr(bstrName);
        SysFreeString(bstrName);
    }

    /* States */
    extract_states(elem, node);

    /* Bounding rectangle */
    RECT rect;
    if (SUCCEEDED(elem->get_CachedBoundingRectangle(&rect))) {
        node->x = rect.left;
        node->y = rect.top;
        node->w = rect.right - rect.left;
        node->h = rect.bottom - rect.top;
    }

    if (depth < MD_UIA_MAX_DEPTH && cached != UIA_CACHED_ELEMENT &&
        !(cached == UIA_CACHED_BOUNDED
              ? walk_children_bounded(st, w, elem, depth, node)
              : walk_children(st, w, elem, depth, cached, node))) {
        md_a11y_node_free(node);
        return nullptr;
    }

    return node;
}

/* Navigate to a top-level sibling (or first child when `first`) of `from`.
 * With no wide window on screen, the navigation prefetches the whole subtree
 * in the same round trip. If that fails, the fetch rather than the
 * navigation may be at fault: navigate again with only the element's
 * properties, then try a per-level refresh (leaving the window a leaf if
 * even that fails). With a wide window on screen, every window is navigated
 * element-only first: a wide one is walked by bounded navigation, any other
 * is refreshed with the subtree request, then the per-level one. Only a
 * failed element-only navigation ends enumeration, as a failed uncached
 * GetNextSiblingElement did. */
static HRESULT walk_top_level_step(const UIAWalk *w, IUIAutomationElement *from,
                                   bool first, IUIAutomationElement **out,
                                   UIACached *cached) {
    IUIAutomationTreeWalker *walker = w->walker;
    *out = nullptr;
    HRESULT hr;
    if (!w->wide_count) {
        *cached = UIA_CACHED_SUBTREE;
        hr = uia_fetch_rejected(w->subtree) ? E_FAIL : first
            ? walker->GetFirstChildElementBuildCache(from, w->subtree, out)
            : walker->GetNextSiblingElementBuildCache(from, w->subtree, out);
        if (SUCCEEDED(hr)) return hr;
        SAFE_RELEASE(*out);
    }

    *cached = UIA_CACHED_ELEMENT;
    hr = first
        ? walker->GetFirstChildElementBuildCache(from, w->element, out)
        : walker->GetNextSiblingElementBuildCache(from, w->element, out);
    if (FAILED(hr)) {
        SAFE_RELEASE(*out);
        return hr;
    }
    if (!*out) return hr;
    if (w->wide_count) {
        if (uia_is_wide(w, *out)) {
            *cached = UIA_CACHED_BOUNDED;
            return hr;
        }
        if (uia_refresh(w->subtree, out)) {
            *cached = UIA_CACHED_SUBTREE;
            return hr;
        }
    }
    if (uia_refresh(w->level, out)) *cached = UIA_CACHED_CHILDREN;
    return hr;
}

/* ── Vtable implementation ───────────────────────────────────── */

static int uia_init(MdA11yCtx *ctx) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int com_initialized = 0;
    if (SUCCEEDED(hr)) {
        com_initialized = 1;
    } else {
        /* Do not publish STA-owned proxies to the bridge's worker threads. */
        MD_LOG_I("CoInitializeEx(MTA) failed: 0x%08lx", hr);
        return -1;
    }

    auto *st = (UIAState *)calloc(1, sizeof(UIAState));
    if (!st) {
        if (com_initialized) CoUninitialize();
        return -1;
    }
    st->com_initialized = com_initialized;
    st->ctx = ctx;
    InitializeCriticalSection(&st->lock);
    st->lock_initialized = 1;
    InitializeSRWLock(&st->calls_lock);
    InitializeConditionVariable(&st->calls_drained);

    hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                          CLSCTX_INPROC_SERVER,
                          __uuidof(IUIAutomation),
                          (void **)&st->automation);
    if (FAILED(hr) || !st->automation) {
        MD_LOG_E("failed to create IUIAutomation: 0x%08lx", hr);
        if (st->lock_initialized) DeleteCriticalSection(&st->lock);
        if (st->com_initialized) CoUninitialize();
        free(st);
        return -1;
    }

    /* Create a content tree walker (skips raw/control elements) */
    hr = st->automation->get_ContentViewWalker(&st->walker);
    if (FAILED(hr) || !st->walker) {
        MD_LOG_E("failed to get ContentViewWalker");
        SAFE_RELEASE(st->automation);
        if (st->lock_initialized) DeleteCriticalSection(&st->lock);
        if (st->com_initialized) CoUninitialize();
        free(st);
        return -1;
    }

    InterlockedExchange(&st->connected, 1);
    IUIAutomationElement *root = nullptr;
    if (FAILED(uia_root(st, st->automation, &root))) uia_lost(st);
    SAFE_RELEASE(root);
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &st->session_id))
        uia_lost(st);
    st->wts_module = LoadLibraryW(L"wtsapi32.dll");
    if (st->wts_module) {
        /* memcpy avoids MSVC /W4's incompatible function-pointer cast warning.
         * FARPROC and these Win32 function pointers have identical size. */
#define MD_WTS_PROC(member, name) do { \
        FARPROC proc = GetProcAddress(st->wts_module, name); \
        static_assert(sizeof(st->member) == sizeof(proc), "Win32 function pointer size"); \
        memcpy(&st->member, &proc, sizeof(proc)); \
    } while (0)
        MD_WTS_PROC(wts_query, "WTSQuerySessionInformationW");
        MD_WTS_PROC(wts_free, "WTSFreeMemory");
        MD_WTS_PROC(wts_register, "WTSRegisterSessionNotification");
        MD_WTS_PROC(wts_unregister, "WTSUnRegisterSessionNotification");
#undef MD_WTS_PROC
    }
    st->session_stop = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    st->session_ready = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (st->session_stop && st->session_ready && st->wts_query && st->wts_free &&
        st->wts_register && st->wts_unregister)
        st->session_thread = CreateThread(nullptr, 0, uia_session_thread_proc, st, 0, nullptr);
    if (st->session_thread)
        WaitForSingleObject(st->session_ready, INFINITE);
    if (!st->session_thread || !st->session_registered) {
        uia_stop_session_thread(st);
        SAFE_RELEASE(st->walker);
        SAFE_RELEASE(st->automation);
        DeleteCriticalSection(&st->lock);
        if (st->com_initialized) CoUninitialize();
        free(st);
        return -1;
    }
    ctx->backend_data = st;
    return 0;
}

/* Attach the walker children of `parent` (the desktop root in production) to
 * `node` as depth-`depth` nodes. `parent_hwnd` is parent's window, or
 * nullptr for the desktop. Returns false only on allocation failure. */
static bool walk_top_level(UIAState *st, UIAWalk *w, IUIAutomationElement *parent,
                           HWND parent_hwnd, int depth, MdA11yNode *node) {
    uia_scan_wide(w, parent_hwnd);
    IUIAutomationElement *child = nullptr;
    UIACached cached = UIA_CACHED_SUBTREE;
    HRESULT hr = walk_top_level_step(w, parent, true, &child, &cached);
    if (FAILED(hr) || !child) {
        SAFE_RELEASE(child);
        return true;
    }

    /* Heap-allocated: up to MD_UIA_MAX_CHILDREN pointers. */
    MdA11yNode **children =
        (MdA11yNode **)calloc(MD_UIA_MAX_CHILDREN, sizeof(MdA11yNode *));
    if (!children) {
        child->Release();
        return false;
    }
    int childCount = 0;

    while (child && childCount < MD_UIA_MAX_CHILDREN) {
        MdA11yNode *childNode = walk_node(st, w, child, depth, cached);
        if (childNode)
            children[childCount++] = childNode;

        /* Prefetching past the child cap would fetch a whole subtree only
         * to drop it. */
        if (childCount == MD_UIA_MAX_CHILDREN) break;
        IUIAutomationElement *next = nullptr;
        hr = walk_top_level_step(w, child, false, &next, &cached);
        child->Release();
        child = (SUCCEEDED(hr)) ? next : nullptr;
    }
    /* Exiting on the child cap leaves `child` held */
    SAFE_RELEASE(child);

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
    free(children);
    return true;
}

static int uia_get_tree_with_unlocked(UIAState *st, IUIAutomation *automation,
                                      IUIAutomationTreeWalker *walker,
                                      MdA11yNode **out_root) {
    if (!st || !automation || !walker || !uia_connected(st) || !out_root)
        return -1;

    *out_root = nullptr;
    st->next_id = 0;

    /* Get the root element (desktop). This is the liveness probe: root
     * provider failures latch loss inside uia_root(). */
    IUIAutomationElement *rootElem = nullptr;
    HRESULT hr = uia_root(st, automation, &rootElem);
    if (FAILED(hr) || !rootElem) return -1;

    UIAWalk w;
    if (FAILED(uia_walk_begin(automation, walker, &w))) {
        rootElem->Release();
        return -1;
    }

    /* Create root node */
    MdA11yNode *root = (MdA11yNode *)calloc(1, sizeof(MdA11yNode));
    if (!root) {
        uia_walk_end(&w);
        rootElem->Release();
        return -1;
    }

    root->id = _strdup("desktop");
    root->role = _strdup(md_a11y_role_name(MD_A11Y_ROLE_DESKTOP));
    root->label = _strdup("Desktop");

    /* Walk top-level children (application windows) */
    bool ok = walk_top_level(st, &w, rootElem, nullptr, 1, root);
    rootElem->Release();
    uia_walk_end(&w);
    if (!ok) {
        md_a11y_node_free(root);
        return -1;
    }

    *out_root = root;
    return 0;
}

static int uia_get_tree(MdA11yCtx *ctx, MdA11yNode **out_root) {
    auto *st = ctx ? (UIAState *)ctx->backend_data : nullptr;
    if (!st || !st->lock_initialized) return -1;

    AcquireSRWLockExclusive(&st->calls_lock);
    if (st->closing) {
        ReleaseSRWLockExclusive(&st->calls_lock);
        return -1;
    }
    st->active_calls++;
#ifdef MD_UIA_TESTING
    if (uia_test_call_admitted) ReleaseSemaphore(uia_test_call_admitted, 1, nullptr);
#endif
    ReleaseSRWLockExclusive(&st->calls_lock);

    /* Bridge callers may be plain worker threads. Join the MTA for the whole
     * call rather than using a proxy on an uninitialized or STA thread. */
    HRESULT apartment_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int ret = -1;
    if (SUCCEEDED(apartment_hr)) {
        EnterCriticalSection(&st->lock);
        ret = uia_get_tree_with_unlocked(st, st->automation, st->walker, out_root);
        LeaveCriticalSection(&st->lock);
        CoUninitialize();
    }

    AcquireSRWLockExclusive(&st->calls_lock);
    if (--st->active_calls == 0) WakeAllConditionVariable(&st->calls_drained);
    ReleaseSRWLockExclusive(&st->calls_lock);
    return ret;
}

class UIAChangeHandler final : public IUIAutomationFocusChangedEventHandler,
                               public IUIAutomationStructureChangedEventHandler,
                               public IUIAutomationEventHandler {
public:
    UIAChangeHandler(UIAState *st, IUIAutomation *automation,
                     IUIAutomationTreeWalker *walker)
        : refs_(1), st_(st), automation_(automation), walker_(walker) {
        InitializeCriticalSection(&callback_lock_);
        if (automation_) automation_->AddRef();
        if (walker_) walker_->AddRef();
    }

    /* Not virtual: the class is final and Release() deletes via its own
     * static type. COM interfaces carry no virtual destructor. */
    ~UIAChangeHandler() {
        DeleteCriticalSection(&callback_lock_);
        SAFE_RELEASE(walker_);
        SAFE_RELEASE(automation_);
    }

    /* COM may retain this handler and call it after unregistration. Detach
     * while holding a handler-owned lock, draining any dispatch using st_. */
    void detach() {
        EnterCriticalSection(&callback_lock_);
        st_ = nullptr;
        LeaveCriticalSection(&callback_lock_);
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
        EnterCriticalSection(&callback_lock_);
        if (st_ && st_->ctx && uia_connected(st_))
            md_a11y_change_dispatch(st_->ctx);
        LeaveCriticalSection(&callback_lock_);
    }

    CRITICAL_SECTION callback_lock_;
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
        hr = uia_root(st, automation, &root);
    if (SUCCEEDED(hr) && !root)
        hr = E_FAIL;
    if (SUCCEEDED(hr) && automation && walker)
        handler = new (std::nothrow) UIAChangeHandler(st, automation, walker);
    if (SUCCEEDED(hr) && !handler)
        hr = E_OUTOFMEMORY;

    if (SUCCEEDED(hr)) {
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

    if (handler) handler->detach();
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

static void uia_teardown_subscription(UIAState *st);

static int uia_subscribe_changes(MdA11yCtx *ctx, MdA11yChangeCb cb,
                                 void *userdata) {
    auto *st = ctx ? (UIAState *)ctx->backend_data : nullptr;
    if (!st || !uia_connected(st) || !st->lock_initialized || !cb)
        return -1;
    (void)userdata; /* callback + userdata are stored on ctx by a11y.c */

    EnterCriticalSection(&st->lock);
    if (st->subscribed) {
        /* Handlers already registered; a11y.c swapped the callback. */
        LeaveCriticalSection(&st->lock);
        return 0;
    }
    if (st->event_thread) {
        LeaveCriticalSection(&st->lock);
        return -1;
    }
    st->subscribe_hr = E_PENDING;
    LeaveCriticalSection(&st->lock);

    st->stop_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    st->ready_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!st->stop_event || !st->ready_event) {
        if (st->stop_event) CloseHandle(st->stop_event);
        if (st->ready_event) CloseHandle(st->ready_event);
        st->stop_event = nullptr;
        st->ready_event = nullptr;
        return -1;
    }

    st->event_thread = CreateThread(nullptr, 0, uia_event_thread_proc,
                                    st, 0, nullptr);
    if (!st->event_thread) {
        CloseHandle(st->stop_event);
        CloseHandle(st->ready_event);
        st->stop_event = nullptr;
        st->ready_event = nullptr;
        return -1;
    }

    WaitForSingleObject(st->ready_event, INFINITE);

    HRESULT hr;
    EnterCriticalSection(&st->lock);
    hr = st->subscribe_hr;
    LeaveCriticalSection(&st->lock);

    if (FAILED(hr)) {
        uia_teardown_subscription(st);
        MD_LOG_E("failed to subscribe UIA events: 0x%08lx", hr);
        return -1;
    }

    return 0;
}

/* Single teardown path shared by unsubscribe, the subscribe failure path
 * and destroy. Signals the event thread to stop and joins it; on its way
 * out that thread calls RemoveStructureChangedEventHandler /
 * RemoveFocusChangedEventHandler for whatever it registered and releases
 * the UIAChangeHandler (see uia_event_thread_proc), so the join is what
 * guarantees UIA will not call back into us afterwards. The user callback
 * is not touched here — a11y.c has already cleared and drained it before
 * calling us. Idempotent. */
static void uia_teardown_subscription(UIAState *st) {
    if (!st) return;

    if (st->lock_initialized) {
        EnterCriticalSection(&st->lock);
        st->subscribed = 0;
        LeaveCriticalSection(&st->lock);
    }

    if (st->event_thread) {
        /* No quit-before-loop race here, unlike the GLib/CFRunLoop
         * backends: stop_event is a manual-reset event, so the signal is
         * sticky and MsgWaitForMultipleObjects returns immediately even if
         * the thread has not reached its wait yet. Subscribe also blocks on
         * ready_event, so by the time anyone can tear down, the thread is
         * already in (or past) its wait loop. */
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
}

static void uia_unsubscribe_changes(MdA11yCtx *ctx) {
    auto *st = ctx ? (UIAState *)ctx->backend_data : nullptr;
    if (!st) return;
    uia_teardown_subscription(st);
}

static void uia_destroy(MdA11yCtx *ctx) {
    auto *st = ctx ? (UIAState *)ctx->backend_data : nullptr;
    if (!st) return;

    uia_lost(st);
    AcquireSRWLockExclusive(&st->calls_lock);
    st->closing = true;
#ifdef MD_UIA_TESTING
    if (uia_test_destroy_waiting) SetEvent(uia_test_destroy_waiting);
#endif
    /* A critical section is not FIFO: merely acquiring the tree lock here
     * could overtake and free a caller already queued on it. Drain all admitted
     * calls, including lock waiters, before deleting any backend state. */
    while (st->active_calls)
        SleepConditionVariableSRW(&st->calls_drained, &st->calls_lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&st->calls_lock);
    uia_teardown_subscription(st);
    uia_stop_session_thread(st);

    /* The owner stops starting new calls before destroy (the supervised
     * bridge joins its watcher). Admitted tree calls have drained above; never
     * join an event thread with the tree lock held. */
    EnterCriticalSection(&st->lock);
    SAFE_RELEASE(st->walker);
    SAFE_RELEASE(st->automation);
    LeaveCriticalSection(&st->lock);

    if (st->lock_initialized)
        DeleteCriticalSection(&st->lock);

    if (st->com_initialized)
        CoUninitialize();

    free(st);
    ctx->backend_data = nullptr;
}

/* ── Singleton vtable ────────────────────────────────────────── */

static bool uia_is_connected(const MdA11yCtx *ctx) {
    auto *st = ctx ? static_cast<UIAState *>(ctx->backend_data) : nullptr;
    return uia_connected(st);
}

/* Positional: must match the MdA11yBackend member order in a11y.h. */
static const MdA11yBackend uia_backend = {
    uia_init,
    uia_get_tree,
    uia_subscribe_changes,
    uia_unsubscribe_changes,
    uia_destroy,
    uia_is_connected,
};

extern "C"
const MdA11yBackend *md_a11y_backend_create(void) {
    return &uia_backend;
}

#else /* !_WIN32 */

extern "C"
const MdA11yBackend *md_a11y_backend_create(void) {
    MD_LOG_I("UI Automation backend not available on this platform");
    return nullptr;
}

#endif /* _WIN32 */
