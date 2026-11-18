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

/* Extract states from UIA element properties */
static void extract_states(IUIAutomationElement *elem, MdA11yNode *node) {
    char *states[8];
    int count = 0;

    BOOL boolVal;

    if (SUCCEEDED(elem->get_CurrentIsEnabled(&boolVal)) && boolVal)
        states[count++] = _strdup(md_a11y_state_name(MD_A11Y_STATE_ENABLED));

    if (SUCCEEDED(elem->get_CurrentHasKeyboardFocus(&boolVal)) && boolVal)
        states[count++] = _strdup(md_a11y_state_name(MD_A11Y_STATE_FOCUSED));

    if (SUCCEEDED(elem->get_CurrentIsKeyboardFocusable(&boolVal)) && boolVal)
        states[count++] = _strdup(md_a11y_state_name(MD_A11Y_STATE_FOCUSABLE));

    if (SUCCEEDED(elem->get_CurrentIsOffscreen(&boolVal)) && !boolVal) {
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

/* ── Tree walking ────────────────────────────────────────────── */

static MdA11yNode *walk_element(UIAState *st, IUIAutomationTreeWalker *walker,
                                 IUIAutomationElement *elem, int depth) {
    if (!st || !walker || !elem || depth > MD_UIA_MAX_DEPTH)
        return nullptr;

    MdA11yNode *node = (MdA11yNode *)calloc(1, sizeof(MdA11yNode));
    if (!node) return nullptr;

    node->id = make_node_id(st);

    /* Prefer the platform's stable element identity over DFS position.
     * UIA RuntimeId survives across walks; the DFS counter does not. */
    {
        SAFEARRAY *rid = nullptr;
        if (SUCCEEDED(elem->GetRuntimeId(&rid)) && rid) {
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
            SafeArrayDestroy(rid);
            if (off > 2) {
                free(node->id);
                node->id = _strdup(buf);
            }
        }
    }

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
        /* Heap-allocated: with recursion depth MD_UIA_MAX_DEPTH the
         * stack copies would total ~64 KB. */
        MdA11yNode **children =
            (MdA11yNode **)calloc(MD_UIA_MAX_CHILDREN, sizeof(MdA11yNode *));
        if (!children) {
            SAFE_RELEASE(child);
            md_a11y_node_free(node);
            return nullptr;
        }
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
    }

    return node;
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

static int uia_get_tree_with_unlocked(UIAState *st, IUIAutomation *automation,
                                      IUIAutomationTreeWalker *walker,
                                      MdA11yNode **out_root) {
    if (!st || !automation || !walker || !uia_connected(st) || !out_root)
        return -1;

    *out_root = nullptr;
    st->next_id = 0;

    /* Get the root element (desktop) */
    IUIAutomationElement *rootElem = nullptr;
    HRESULT hr = uia_root(st, automation, &rootElem);
    if (FAILED(hr) || !rootElem) return -1;

    /* Create root node */
    MdA11yNode *root = (MdA11yNode *)calloc(1, sizeof(MdA11yNode));
    if (!root) {
        rootElem->Release();
        return -1;
    }

    root->id = _strdup("desktop");
    root->role = _strdup(md_a11y_role_name(MD_A11Y_ROLE_DESKTOP));
    root->label = _strdup("Desktop");

    /* Walk top-level children (application windows) */
    IUIAutomationElement *child = nullptr;
    hr = walker->GetFirstChildElement(rootElem, &child);
    rootElem->Release();

    if (SUCCEEDED(hr) && child) {
        MdA11yNode **children =
            (MdA11yNode **)calloc(MD_UIA_MAX_CHILDREN, sizeof(MdA11yNode *));
        if (!children) {
            SAFE_RELEASE(child);
            md_a11y_node_free(root);
            *out_root = nullptr;
            return -1;
        }
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
        /* Exiting on the child cap leaves `child` held */
        SAFE_RELEASE(child);

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
        free(children);
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
