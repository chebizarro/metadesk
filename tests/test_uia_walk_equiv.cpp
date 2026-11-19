/* metadesk — tests/test_uia_walk_equiv.cpp (md-cw8)
 *
 * Standalone Windows harness: the cache-request UIA walk must produce exactly
 * the tree the uncached TreeWalker walk produced (same nodes, ids, roles,
 * names, states and bounds, in the same order). The reference walker below
 * is a verbatim copy of the pre-md-cw8 walk_element() using Current*
 * properties; it lives only in this test translation unit.
 *
 *   test_uia_walk_equiv.exe --interactive   fixture + real desktop cells with
 *                                           timings (console session only)
 *   test_uia_walk_equiv.exe                 same, but SKIPs cells that need a
 *                                           visible desktop: SSH session 0 runs
 *                                           on a non-interactive window station
 *                                           where no window is ever visible, so
 *                                           UIA exposes no fixture children.
 *
 * The fixture is a Win32 window owned by a helper thread: buttons (one
 * disabled), a checkbox, an edit, a label, a 300-item list box (exercises the
 * MD_UIA_MAX_CHILDREN cap) and a chain of nested windows deeper than
 * MD_UIA_MAX_DEPTH (exercises the depth cap). Both the subtree-cache path and
 * the per-level fallback path are compared against the reference.
 */
#define MD_UIA_TESTING 1
#include "../src/core/a11y_uia.cpp"

#include <algorithm>
#include <string>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s (winerr=%lu)\n", __FILE__, __LINE__, #expr, GetLastError()); \
    ExitProcess(1); \
} } while (0)

/* ── Reference: uncached walk (pre-md-cw8 production code, verbatim) ── */

static void ref_extract_states(IUIAutomationElement *elem, MdA11yNode *node) {
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

static MdA11yNode *ref_walk_element(UIAState *st, IUIAutomationTreeWalker *walker,
                                    IUIAutomationElement *elem, int depth) {
    if (!st || !walker || !elem || depth > MD_UIA_MAX_DEPTH)
        return nullptr;

    MdA11yNode *node = (MdA11yNode *)calloc(1, sizeof(MdA11yNode));
    if (!node) return nullptr;

    node->id = make_node_id(st);

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

    CONTROLTYPEID ctid = 0;
    elem->get_CurrentControlType(&ctid);
    node->role = _strdup(control_type_to_role(ctid));

    BSTR bstrName = nullptr;
    if (SUCCEEDED(elem->get_CurrentName(&bstrName)) && bstrName) {
        node->label = bstr_to_cstr(bstrName);
        SysFreeString(bstrName);
    }

    ref_extract_states(elem, node);

    RECT rect;
    if (SUCCEEDED(elem->get_CurrentBoundingRectangle(&rect))) {
        node->x = rect.left;
        node->y = rect.top;
        node->w = rect.right - rect.left;
        node->h = rect.bottom - rect.top;
    }

    IUIAutomationElement *child = nullptr;
    HRESULT hr = walker->GetFirstChildElement(elem, &child);
    if (SUCCEEDED(hr) && child) {
        MdA11yNode **children =
            (MdA11yNode **)calloc(MD_UIA_MAX_CHILDREN, sizeof(MdA11yNode *));
        if (!children) {
            SAFE_RELEASE(child);
            md_a11y_node_free(node);
            return nullptr;
        }
        int childCount = 0;

        while (child && childCount < MD_UIA_MAX_CHILDREN) {
            MdA11yNode *childNode = ref_walk_element(st, walker, child, depth + 1);
            if (childNode)
                children[childCount++] = childNode;

            IUIAutomationElement *next = nullptr;
            hr = walker->GetNextSiblingElement(child, &next);
            child->Release();
            child = (SUCCEEDED(hr)) ? next : nullptr;
        }
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

/* Pre-md-cw8 uia_get_tree_with_unlocked() for the desktop root. */
static MdA11yNode *ref_walk_desktop(UIAState *st) {
    st->next_id = 0;
    IUIAutomationElement *rootElem = nullptr;
    if (FAILED(uia_root(st, st->automation, &rootElem)) || !rootElem) return nullptr;
    MdA11yNode *root = (MdA11yNode *)calloc(1, sizeof(MdA11yNode));
    CHECK(root);
    root->id = _strdup("desktop");
    root->role = _strdup(md_a11y_role_name(MD_A11Y_ROLE_DESKTOP));
    root->label = _strdup("Desktop");
    IUIAutomationElement *child = nullptr;
    HRESULT hr = st->walker->GetFirstChildElement(rootElem, &child);
    rootElem->Release();
    std::vector<MdA11yNode *> children;
    while (SUCCEEDED(hr) && child && children.size() < MD_UIA_MAX_CHILDREN) {
        MdA11yNode *childNode = ref_walk_element(st, st->walker, child, 1);
        if (childNode) children.push_back(childNode);
        IUIAutomationElement *next = nullptr;
        hr = st->walker->GetNextSiblingElement(child, &next);
        child->Release();
        child = SUCCEEDED(hr) ? next : nullptr;
    }
    SAFE_RELEASE(child);
    if (!children.empty()) {
        root->children = (MdA11yNode **)calloc(children.size(), sizeof(MdA11yNode *));
        CHECK(root->children);
        std::copy(children.begin(), children.end(), root->children);
        root->child_count = (int)children.size();
    }
    return root;
}

/* ── Comparison ─────────────────────────────────────────────── */

static std::string str_or_null(const char *s) { return s ? "\"" + std::string(s) + "\"" : "null"; }

static bool same_node(const MdA11yNode *a, const MdA11yNode *b,
                      const std::string &path, std::string &why) {
    auto differ = [&](const char *field, const std::string &x, const std::string &y) {
        why = path + "." + field + ": uncached=" + x + " cached=" + y;
        return false;
    };
    auto eq = [](const char *x, const char *y) {
        return (!x && !y) || (x && y && strcmp(x, y) == 0);
    };
    if (!eq(a->id, b->id)) return differ("id", str_or_null(a->id), str_or_null(b->id));
    if (!eq(a->role, b->role)) return differ("role", str_or_null(a->role), str_or_null(b->role));
    if (!eq(a->label, b->label)) return differ("label", str_or_null(a->label), str_or_null(b->label));
    if (a->state_count != b->state_count)
        return differ("state_count", std::to_string(a->state_count), std::to_string(b->state_count));
    for (int i = 0; i < a->state_count; i++)
        if (!eq(a->states[i], b->states[i]))
            return differ("state", str_or_null(a->states[i]), str_or_null(b->states[i]));
    if (a->x != b->x || a->y != b->y || a->w != b->w || a->h != b->h) {
        char x[96], y[96];
        snprintf(x, sizeof(x), "%d,%d %dx%d", a->x, a->y, a->w, a->h);
        snprintf(y, sizeof(y), "%d,%d %dx%d", b->x, b->y, b->w, b->h);
        return differ("bounds", x, y);
    }
    if (a->child_count != b->child_count)
        return differ("child_count", std::to_string(a->child_count), std::to_string(b->child_count));
    for (int i = 0; i < a->child_count; i++)
        if (!same_node(a->children[i], b->children[i],
                       path + "/" + (a->children[i]->id ? a->children[i]->id : "?"), why))
            return false;
    return true;
}

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

/* Structural equality, then byte equality of both wire formats. */
static bool same_tree(const MdA11yNode *a, const MdA11yNode *b, std::string &why,
                      uint64_t *json_hash = nullptr) {
    if (!same_node(a, b, a->id ? a->id : "?", why)) return false;
    char *ja = md_a11y_to_json_at(a, 0), *jb = md_a11y_to_json_at(b, 0);
    char *ca = md_a11y_to_compact_at(a, 0), *cb = md_a11y_to_compact_at(b, 0);
    CHECK(ja && jb && ca && cb);
    bool same = strcmp(ja, jb) == 0 && strcmp(ca, cb) == 0;
    if (!same) why = "serialised JSON/compact output differs";
    if (json_hash) *json_hash = fnv1a(ja);
    free(ja); free(jb); free(ca); free(cb);
    return same;
}

static int count_nodes(const MdA11yNode *n) {
    int c = 1;
    for (int i = 0; i < n->child_count; i++) c += count_nodes(n->children[i]);
    return c;
}

static int max_depth(const MdA11yNode *n, int depth) {
    int d = depth;
    for (int i = 0; i < n->child_count; i++) /* parenthesised: windows.h max() */
        d = (std::max)(d, max_depth(n->children[i], depth + 1));
    return d;
}

static const MdA11yNode *find_label(const MdA11yNode *n, const char *label) {
    if (n->label && strcmp(n->label, label) == 0) return n;
    for (int i = 0; i < n->child_count; i++)
        if (const MdA11yNode *f = find_label(n->children[i], label)) return f;
    return nullptr;
}

static double now_ms() {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

/* ── Fixture window ─────────────────────────────────────────── */

#ifndef FIXTURE_LIST_ITEMS /* override to measure overfetch on very wide lists */
#define FIXTURE_LIST_ITEMS 300
#endif
#define FIXTURE_NEST_LEVELS (MD_UIA_MAX_DEPTH + 4)

struct Fixture {
    HANDLE ready;
    HWND window;
    HWND list;
    DWORD thread_id;
    bool visible;
};

static LRESULT CALLBACK fixture_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, msg, wp, lp);
}

/* The window lives on its own pumping thread: UIA client calls must not be
 * made from the thread that owns the window being queried. */
static DWORD WINAPI fixture_thread(LPVOID data) {
    auto *fx = static_cast<Fixture *>(data);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = fixture_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"MetadeskUIAWalkFixture";
    CHECK(RegisterClassW(&wc));
    /* No caption: keeps title-bar MSAA proxies (system menu) out of the
     * fixture so it contains only the controls built below. */
    HWND top = CreateWindowExW(0, wc.lpszClassName, L"md-cw8 walk fixture",
        WS_POPUP | WS_BORDER, 40, 40, 720, 560, nullptr, nullptr, inst, nullptr);
    CHECK(top);
    auto child = [&](const wchar_t *cls, const wchar_t *text, DWORD style,
                     int x, int y, int w, int h, HWND parent) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                 x, y, w, h, parent, nullptr, inst, nullptr);
        CHECK(c);
        return c;
    };
    child(L"BUTTON", L"Push", BS_PUSHBUTTON | WS_TABSTOP, 10, 10, 100, 28, top);
    child(L"BUTTON", L"Disabled", BS_PUSHBUTTON | WS_DISABLED, 120, 10, 100, 28, top);
    child(L"BUTTON", L"Check", BS_AUTOCHECKBOX | WS_TABSTOP, 230, 10, 100, 28, top);
    child(L"EDIT", L"editable text", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 340, 10, 160, 28, top);
    child(L"STATIC", L"Plain label", SS_LEFT, 510, 10, 180, 28, top);
    HWND list = child(L"LISTBOX", nullptr, WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                      10, 50, 200, 460, top);
    for (int i = 0; i < FIXTURE_LIST_ITEMS; i++) {
        wchar_t item[32];
        swprintf(item, 32, L"item %d", i);
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    HWND parent = top;
    for (int level = 0; level < FIXTURE_NEST_LEVELS; level++) {
        wchar_t name[32];
        swprintf(name, 32, L"nest %d", level);
        parent = child(L"BUTTON", name, BS_GROUPBOX, level == 0 ? 230 : 4,
                       level == 0 ? 50 : 10, 460 - 8 * level, 460 - 8 * level, parent);
    }
    /* A process started by a scheduled task or SSH may carry SW_HIDE in its
     * STARTUPINFO, which the first ShowWindow honours over its argument. */
    ShowWindow(top, SW_SHOWNORMAL);
    ShowWindow(top, SW_SHOWNORMAL);
    UpdateWindow(top);
    fx->visible = IsWindowVisible(top) != 0;
    fx->window = top;
    fx->list = list;
    SetEvent(fx->ready);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}

/* Cached walk of one element, through the same walk_node() production uses
 * for a top-level window, from either the subtree or the per-level cache. */
static MdA11yNode *cached_walk(UIAState *st, IUIAutomationElement *elem, UIACached mode) {
    UIAWalk w;
    CHECK(SUCCEEDED(uia_walk_begin(st->automation, &w)));
    IUIAutomationElement *cached = nullptr;
    CHECK(SUCCEEDED(elem->BuildUpdatedCache(
        mode == UIA_CACHED_SUBTREE ? w.subtree : w.level, &cached)) && cached);
    st->next_id = 0;
    MdA11yNode *node = walk_node(st, &w, cached, 1, mode);
    cached->Release();
    uia_walk_end(&w);
    CHECK(node);
    return node;
}

static MdA11yNode *ref_walk(UIAState *st, IUIAutomationElement *elem) {
    st->next_id = 0;
    MdA11yNode *node = ref_walk_element(st, st->walker, elem, 1);
    CHECK(node);
    return node;
}

/* Production walk_top_level() over `parent`'s children at depth 2, numbered
 * as if `parent` were node n0 at depth 1 (matching ref_walk(parent)). */
static MdA11yNode *top_level_walk(UIAState *st, const UIAWalk *w, IUIAutomationElement *parent) {
    MdA11yNode *holder = (MdA11yNode *)calloc(1, sizeof(MdA11yNode));
    CHECK(holder);
    st->next_id = 1;
    CHECK(walk_top_level(st, st->walker, w, parent, 2, holder));
    return holder;
}

static void check_children(const MdA11yNode *ref, const MdA11yNode *got, const char *cell) {
    std::string why;
    CHECK(ref->child_count == got->child_count);
    for (int i = 0; i < ref->child_count; i++)
        if (!same_node(ref->children[i], got->children[i], ref->children[i]->id, why)) {
            fprintf(stderr, "%s: %s\n", cell, why.c_str());
            CHECK(false);
        }
    printf("PASS fixture: %s (%d top-level nodes)\n", cell, got->child_count);
}

/* The enumeration/fallback tiers of walk_top_level_step(), and its child-cap
 * break, driven against fixture elements standing in for the desktop root. */
static void test_top_level_paths(UIAState *st, IUIAutomationElement *window,
                                 IUIAutomationElement *list, const MdA11yNode *ref) {
    UIAWalk w;
    CHECK(SUCCEEDED(uia_walk_begin(st->automation, &w)));
    MdA11yNode *got = top_level_walk(st, &w, window);
    check_children(ref, got, "top-level subtree tier == uncached");
    md_a11y_node_free(got);

    uia_test_rejected[0] = w.subtree;
    got = top_level_walk(st, &w, window);
    check_children(ref, got, "subtree fetch rejected -> element nav + per-level walk == uncached");
    md_a11y_node_free(got);

    uia_test_rejected[1] = w.level;
    got = top_level_walk(st, &w, window);
    /* Every fetch beyond the element fails: enumeration must still reach every
     * window, each emitted as a leaf with its own properties. */
    CHECK(got->child_count == ref->child_count);
    std::string why;
    for (int i = 0; i < ref->child_count; i++) {
        CHECK(got->children[i]->child_count == 0);
        MdA11yNode leaf = *ref->children[i];
        leaf.child_count = 0;
        if (!same_node(&leaf, got->children[i], leaf.id, why)) {
            fprintf(stderr, "leaf fallback: %s\n", why.c_str());
            CHECK(false);
        }
    }
    printf("PASS fixture: subtree and per-level fetch rejected -> enumeration continues (%d leaves)\n",
           got->child_count);
    md_a11y_node_free(got);
    uia_test_rejected[0] = uia_test_rejected[1] = nullptr;

    /* A list wider than the cap as the enumeration parent: the cap break must
     * yield exactly the first MD_UIA_MAX_CHILDREN uncached children. */
    MdA11yNode *list_ref = ref_walk(st, list);
    CHECK(list_ref->child_count == MD_UIA_MAX_CHILDREN);
    got = top_level_walk(st, &w, list);
    check_children(list_ref, got, "top-level child cap == uncached");
    md_a11y_node_free(got);
    md_a11y_node_free(list_ref);
    uia_walk_end(&w);
}

static void report_timing(const char *cell, int nodes,
                          const std::vector<double> &ref, const std::vector<double> &cached) {
    double rm = median(ref), cm = median(cached);
    printf("TIMING %s: %d nodes; uncached median %.1f ms (min %.1f, %.2f ms/node); "
           "cached median %.1f ms (min %.1f, %.3f ms/node); speedup %.1fx\n",
           cell, nodes, rm, *std::min_element(ref.begin(), ref.end()), rm / nodes,
           cm, *std::min_element(cached.begin(), cached.end()), cm / nodes, rm / cm);
}

static void close_fixture(Fixture *fx, HANDLE thread) {
    PostMessageW(fx->window, WM_CLOSE, 0, 0);
    CHECK(WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    CloseHandle(fx->ready);
}

static void test_fixture(UIAState *st, bool interactive) {
    Fixture fx = {};
    fx.ready = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    CHECK(fx.ready);
    HANDLE thread = CreateThread(nullptr, 0, fixture_thread, &fx, 0, &fx.thread_id);
    CHECK(thread);
    CHECK(WaitForSingleObject(fx.ready, 30000) == WAIT_OBJECT_0);
    if (!fx.visible) {
        CHECK(!interactive);
        puts("SKIP fixture: windows are never visible on this (non-interactive) "
             "window station; run --interactive in the console session");
        close_fixture(&fx, thread);
        return;
    }

    IUIAutomationElement *elem = nullptr;
    CHECK(SUCCEEDED(st->automation->ElementFromHandle(fx.window, &elem)) && elem);

    MdA11yNode *ref = ref_walk(st, elem);
    int nodes = count_nodes(ref);
    const MdA11yNode *list = nullptr;
    for (int i = 0; i < ref->child_count && !list; i++)
        if (ref->children[i]->role && strcmp(ref->children[i]->role, "list") == 0)
            list = ref->children[i];
    printf("fixture: %d nodes, depth %d, list children %d\n", nodes,
           max_depth(ref, 1), list ? list->child_count : -1);
    if (nodes < 64) {
        char *dump = md_a11y_to_compact_at(ref, 0);
        printf("fixture tree too small:\n%s", dump ? dump : "");
        free(dump);
    }
    /* The fixture must actually reach both caps, or it proves nothing. */
    CHECK(find_label(ref, "Push") && find_label(ref, "Disabled") &&
          find_label(ref, "Check") && find_label(ref, "Plain label"));
    CHECK(list && list->child_count == MD_UIA_MAX_CHILDREN);
    CHECK(max_depth(ref, 1) == MD_UIA_MAX_DEPTH);

    std::string why;
    MdA11yNode *sub = cached_walk(st, elem, UIA_CACHED_SUBTREE);
    if (!same_tree(ref, sub, why)) { fprintf(stderr, "subtree cache: %s\n", why.c_str()); CHECK(false); }
    puts("PASS fixture: subtree-cached walk == uncached walk (ids, names, states, bounds, caps)");
    MdA11yNode *lvl = cached_walk(st, elem, UIA_CACHED_CHILDREN);
    if (!same_tree(ref, lvl, why)) { fprintf(stderr, "per-level cache: %s\n", why.c_str()); CHECK(false); }
    puts("PASS fixture: per-level fallback walk == uncached walk");
    md_a11y_node_free(sub);
    md_a11y_node_free(lvl);

    IUIAutomationElement *list_elem = nullptr;
    CHECK(SUCCEEDED(st->automation->ElementFromHandle(fx.list, &list_elem)) && list_elem);
    test_top_level_paths(st, elem, list_elem, ref);
    list_elem->Release();

    std::vector<double> tr, tc;
    for (int i = 0; i < 5; i++) {
        double t0 = now_ms();
        MdA11yNode *r = ref_walk(st, elem);
        double t1 = now_ms();
        MdA11yNode *c = cached_walk(st, elem, UIA_CACHED_SUBTREE);
        double t2 = now_ms();
        CHECK(same_tree(ref, c, why));
        tr.push_back(t1 - t0);
        tc.push_back(t2 - t1);
        md_a11y_node_free(r);
        md_a11y_node_free(c);
    }
    report_timing("fixture", nodes, tr, tc);
    md_a11y_node_free(ref);
    elem->Release();
    close_fixture(&fx, thread);
}

/* The real desktop changes underneath us (clock text, focus), so pair each
 * cached walk with an uncached one taken back to back and allow a few retries
 * before declaring a mismatch. */
static void test_desktop(MdA11yCtx *ctx, UIAState *st) {
    CHECK(md_a11y_is_connected(ctx));
    std::string why;
    bool matched = false;
    for (int attempt = 1; attempt <= 5 && !matched; attempt++) {
        MdA11yNode *ref = ref_walk_desktop(st);
        MdA11yNode *cached = nullptr;
        CHECK(ref && uia_get_tree(ctx, &cached) == 0 && cached);
        uint64_t hash = 0;
        matched = same_tree(ref, cached, why, &hash);
        if (matched)
            printf("PASS desktop: production cached walk == uncached walk "
                   "(%d nodes, %d top-level, json fnv1a %016llx, attempt %d)\n",
                   count_nodes(ref), ref->child_count, (unsigned long long)hash, attempt);
        else
            printf("desktop attempt %d differs: %s\n", attempt, why.c_str());
        md_a11y_node_free(ref);
        md_a11y_node_free(cached);
    }
    CHECK(matched);

    std::vector<double> tr, tc;
    int nodes = 0;
    for (int i = 0; i < 7; i++) {
        double t0 = now_ms();
        MdA11yNode *r = ref_walk_desktop(st);
        double t1 = now_ms();
        MdA11yNode *c = nullptr;
        CHECK(uia_get_tree(ctx, &c) == 0 && c && r);
        double t2 = now_ms();
        nodes = count_nodes(c);
        tr.push_back(t1 - t0);
        tc.push_back(t2 - t1);
        md_a11y_node_free(r);
        md_a11y_node_free(c);
    }
    report_timing("desktop", nodes, tr, tc);
    CHECK(md_a11y_is_connected(ctx));
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
    bool interactive = argc > 1 && strcmp(argv[1], "--interactive") == 0;
    MdA11yCtx *ctx = md_a11y_create();
    CHECK(ctx);
    auto *st = static_cast<UIAState *>(ctx->backend_data);
    printf("session=%lu connected=%d\n", st->session_id, md_a11y_is_connected(ctx));
    test_fixture(st, interactive);
    if (interactive) test_desktop(ctx, st);
    else puts("SKIP desktop: session 0 backend is disconnected by design (md-yre)");
    md_a11y_destroy(ctx);
    puts("ALL UIA WALK EQUIVALENCE TESTS PASSED");
    CoUninitialize();
    return 0;
}
