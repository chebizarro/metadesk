/* Standalone Windows harness: private seams are compiled only into this TU. */
#define MD_UIA_TESTING 1
#include "../src/core/a11y_uia.cpp"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s (winerr=%lu)\n", __FILE__, __LINE__, #expr, GetLastError()); \
    ExitProcess(1); \
} } while (0)

static void wait_event(HANDLE event) {
    CHECK(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0);
}

static HRESULT injected_hr;
static HRESULT failed_root(IUIAutomation *, IUIAutomationElement **) {
    return injected_hr;
}

static HANDLE root_entered, root_release;
static HRESULT blocked_root(IUIAutomation *, IUIAutomationElement **) {
    SetEvent(root_entered);
    wait_event(root_release);
    return RPC_E_DISCONNECTED;
}

struct LifetimeTest {
    MdA11yCtx *ctx;
    HANDLE ready, destroy_start, destroyed;
};

static DWORD WINAPI owner_thread(LPVOID data) {
    auto *test = static_cast<LifetimeTest *>(data);
    test->ctx = md_a11y_create();
    CHECK(test->ctx);
    /* Session 0 is deliberately unavailable; enable only the test state to
     * exercise the same locked tree-call/teardown path with a blocked seam. */
    auto *st = static_cast<UIAState *>(test->ctx->backend_data);
    InterlockedExchange(&st->connected, 1);
    SetEvent(test->ready);
    wait_event(test->destroy_start);
    md_a11y_destroy(test->ctx);
    SetEvent(test->destroyed);
    return 0;
}

static DWORD WINAPI walk_thread(LPVOID data) {
    auto *test = static_cast<LifetimeTest *>(data);
    /* The backend must initialize COM for a plain bridge worker itself. */
    MdA11yNode *root = nullptr;
    CHECK(uia_get_tree(test->ctx, &root) == -1);
    CHECK(!root);
    return 0;
}

static void test_inflight_destroy() {
    LifetimeTest test = {};
    test.ready = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    test.destroy_start = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    test.destroyed = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    root_entered = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    root_release = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    uia_test_destroy_waiting = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    uia_test_call_admitted = CreateSemaphore(nullptr, 0, 2, nullptr);
    CHECK(test.ready && test.destroy_start && test.destroyed && root_entered &&
          root_release && uia_test_destroy_waiting && uia_test_call_admitted);
    HANDLE owner = CreateThread(nullptr, 0, owner_thread, &test, 0, nullptr);
    CHECK(owner);
    wait_event(test.ready);
    uia_test_root = blocked_root;
    HANDLE walker = CreateThread(nullptr, 0, walk_thread, &test, 0, nullptr);
    CHECK(walker);
    wait_event(root_entered);
    wait_event(uia_test_call_admitted);
    HANDLE queued_walker = CreateThread(nullptr, 0, walk_thread, &test, 0, nullptr);
    CHECK(queued_walker);
    wait_event(uia_test_call_admitted); /* admitted, queued behind blocked root */
    SetEvent(test.destroy_start);
    wait_event(uia_test_destroy_waiting);
    /* Destroy closed admission but cannot free state while either the blocked
     * root call or the second call queued on the tree lock remains admitted. */
    CHECK(WaitForSingleObject(test.destroyed, 0) == WAIT_TIMEOUT);
    CHECK(!md_a11y_is_connected(test.ctx));
    auto *st = static_cast<UIAState *>(test.ctx->backend_data);
    CHECK(st->automation && st->walker);
    SetEvent(root_release);
    wait_event(walker);
    wait_event(queued_walker);
    wait_event(test.destroyed);
    wait_event(owner);
    uia_test_root = nullptr;
    CloseHandle(owner); CloseHandle(walker); CloseHandle(queued_walker);
    CloseHandle(test.ready); CloseHandle(test.destroy_start); CloseHandle(test.destroyed);
    CloseHandle(root_entered); CloseHandle(root_release);
    CloseHandle(uia_test_destroy_waiting);
    uia_test_destroy_waiting = nullptr;
    CloseHandle(uia_test_call_admitted);
    uia_test_call_admitted = nullptr;
    puts("PASS destroy drains in-flight root call AND queued tree caller");
}

static void test_root_errors(MdA11yCtx *ctx) {
    auto *st = static_cast<UIAState *>(ctx->backend_data);
    const HRESULT errors[] = { RPC_E_DISCONNECTED, static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE),
        CO_E_OBJNOTCONNECTED, HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) };
    uia_test_root = failed_root;
    for (HRESULT error : errors) {
        InterlockedExchange(&st->connected, 1);
        injected_hr = error;
        MdA11yNode *root = nullptr;
        CHECK(uia_get_tree(ctx, &root) == -1);
        CHECK(!root && !md_a11y_is_connected(ctx));
    }
    InterlockedExchange(&st->connected, 1);
    injected_hr = E_OUTOFMEMORY;
    MdA11yNode *root = nullptr;
    CHECK(uia_get_tree(ctx, &root) == -1);
    CHECK(md_a11y_is_connected(ctx));
    injected_hr = S_OK; /* invalid success with a null desktop root */
    CHECK(uia_get_tree(ctx, &root) == -1);
    CHECK(!md_a11y_is_connected(ctx));
    uia_test_root = nullptr;
    puts("PASS root-provider errors latch loss; resource failure does not");
}

static bool hit_occluded, hit_failed;
static HRESULT fake_point_runtime(IUIAutomation *, POINT point, SAFEARRAY **out) {
    CHECK(point.x == 140 && point.y == 220);
    if (hit_failed) return UIA_E_ELEMENTNOTAVAILABLE;
    *out = SafeArrayCreateVector(VT_I4, 0, 2);
    LONG index = 0; int value = 42;
    CHECK(SUCCEEDED(SafeArrayPutElement(*out, &index, &value)));
    index = 1; value = hit_occluded ? 8 : 7;
    CHECK(SUCCEEDED(SafeArrayPutElement(*out, &index, &value)));
    return S_OK;
}

static void test_hit_identity(MdA11yCtx *ctx) {
    auto *st = static_cast<UIAState *>(ctx->backend_data);
    InterlockedExchange(&st->connected, 1);
    uia_test_point_runtime = fake_point_runtime;
    hit_occluded = true;
    CHECK(!md_a11y_target_at_point(ctx, "u:2a.7", 140, 220));
    hit_occluded = false;
    CHECK(md_a11y_target_at_point(ctx, "u:2a.7", 140, 220));
    hit_failed = true;
    CHECK(!md_a11y_target_at_point(ctx, "u:2a.7", 140, 220));
    hit_failed = false;
    uia_test_point_runtime = nullptr;
    puts("PASS native point RuntimeId rejects occluder and provider failure");
}

static void test_session_messages(MdA11yCtx *ctx) {
    auto *st = static_cast<UIAState *>(ctx->backend_data);
    const WPARAM reasons[] = { WTS_SESSION_LOCK, WTS_CONSOLE_DISCONNECT,
        WTS_REMOTE_DISCONNECT, WTS_SESSION_LOGOFF, WTS_SESSION_TERMINATE };
    for (WPARAM reason : reasons) {
        InterlockedExchange(&st->connected, 1);
        SendMessage(st->session_window, WM_WTSSESSION_CHANGE, reason,
                    static_cast<LPARAM>(st->session_id + 1));
        CHECK(md_a11y_is_connected(ctx));
        SendMessage(st->session_window, WM_WTSSESSION_CHANGE, reason,
                    static_cast<LPARAM>(st->session_id));
        CHECK(!md_a11y_is_connected(ctx));
        SendMessage(st->session_window, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK,
                    static_cast<LPARAM>(st->session_id));
        CHECK(!md_a11y_is_connected(ctx));
    }
    puts("PASS session messages latch loss only for this session; no resurrection");
}

static void noop_change(const MdA11yDelta *, int, void *) {}

static DWORD WINAPI sta_thread(LPVOID) {
    CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    CHECK(md_a11y_create() == nullptr);
    CoUninitialize();
    return 0;
}

int main(int argc, char **argv) {
    CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
    bool interactive = argc > 1 && strcmp(argv[1], "--interactive") == 0;
    bool wait_loss = argc > 1 && strcmp(argv[1], "--wait-for-loss") == 0;
    bool unavailable = argc > 1 && strcmp(argv[1], "--expect-unavailable") == 0;
    bool hit_only = argc > 1 && strcmp(argv[1], "--hit-test-only") == 0;
    MdA11yCtx *ctx = md_a11y_create();
    CHECK(ctx);
    auto *st = static_cast<UIAState *>(ctx->backend_data);
    if (unavailable) {
        CHECK(st->session_id != 0 && st->session_registered);
        CHECK(!md_a11y_is_connected(ctx));
        CHECK(md_a11y_walk(ctx) == nullptr);
        printf("PASS unavailable console startup: session=%lu connected=0\n", st->session_id);
        md_a11y_destroy(ctx);
        CoUninitialize();
        return 0;
    }
    if (hit_only) {
        test_hit_identity(ctx);
        md_a11y_destroy(ctx);
        CoUninitialize();
        puts("ALL UIA HIT-TEST FAKE TESTS PASSED");
        return 0;
    }
    printf("session=%lu connected=%d registered=%d\n", st->session_id,
           md_a11y_is_connected(ctx), st->session_registered);
    CHECK(st->session_registered);
    if (interactive || wait_loss) {
        CHECK(st->session_id != 0 && md_a11y_is_connected(ctx));
        MdA11yNode *root = md_a11y_walk(ctx);
        CHECK(root && root->child_count > 0);
        printf("PASS real interactive desktop: %d top-level nodes\n", root->child_count);
        md_a11y_node_free(root);
        CHECK(md_a11y_subscribe_changes(ctx, noop_change, nullptr) == 0);
        CHECK(md_a11y_unsubscribe_changes(ctx) == 0);
        CHECK(md_a11y_is_connected(ctx)); /* monitor survives unsubscribe */
        puts("PASS UIA subscribe/unsubscribe leaves session monitor active");
    } else {
        CHECK(st->session_id == 0 && !md_a11y_is_connected(ctx));
        MdA11yNode *root = nullptr;
        CHECK(uia_get_tree(ctx, &root) == -1 && !root);
        puts("PASS session 0 has no usable desktop despite allocated COM pointers");
    }
    if (wait_loss) {
        uia_test_loss_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        CHECK(uia_test_loss_event);
        puts("READY: lock or disconnect this Windows session within 30 seconds");
        fflush(stdout);
        wait_event(uia_test_loss_event);
        CHECK(!md_a11y_is_connected(ctx));
        CloseHandle(uia_test_loss_event);
        uia_test_loss_event = nullptr;
        puts("PASS real OS session loss");
    } else {
        test_hit_identity(ctx);
        test_root_errors(ctx);
        test_session_messages(ctx);
    }
    auto *handler = new UIAChangeHandler(st, st->automation, st->walker);
    handler->detach();
    md_a11y_destroy(ctx);
    /* Simulate COM delivering an event after unregister and backend destruction. */
    CHECK(handler->HandleFocusChangedEvent(nullptr) == S_OK);
    handler->Release();
    puts("PASS late callback after backend destruction");
    test_inflight_destroy();
    HANDLE sta = CreateThread(nullptr, 0, sta_thread, nullptr, 0, nullptr);
    CHECK(sta);
    wait_event(sta);
    CloseHandle(sta);
    puts("PASS incompatible STA apartment rejected");
    puts("ALL UIA LIVENESS TESTS PASSED");
    CoUninitialize();
    return 0;
}
