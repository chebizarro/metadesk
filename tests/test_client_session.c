/* Real client DM gate and negotiation wait; no renderer, signer or desktop. */
#define main md_client_main_not_run
#define md_nostr_send_session_request test_request_publish
#define md_nostr_get_npub test_client_pubkey
#include "../src/client/main.c"
#undef main
#include "md_test.h"

static const char *host_pk = "host-pinned-by-client";
static ClientCtx *publishing_ctx;
static const char *refusal_reason;
static GoChannel *request_sent;

const char *test_client_pubkey(const MdNostr *nostr) {
    (void)nostr;
    return "fixture-client";
}
int test_request_publish(MdNostr *nostr, const char *peer, const char *json) {
    (void)nostr;
    MD_CHECK(!strcmp(peer, host_pk));
    MdSessionRequest req;
    MD_CHECK(md_session_request_from_json(json, &req) == 0);
    if (request_sent) {
        go_channel_try_send(request_sent, (void *)(uintptr_t)1);
        return 0;
    }
    MdSessionReject reject = {0};
    memcpy(reject.request_id, req.request_id, sizeof(reject.request_id));
    snprintf(reject.reason, sizeof(reject.reason), "%s", refusal_reason);
    char *answer = md_session_reject_to_json(&reject);
    MD_CHECK(answer);
    MdNostrDmInfo info = {.rumor_created_at = req.issued_at, .live = true};
    on_session_dm("wrong-host", answer, &info, publishing_ctx);
    MD_CHECK(!publishing_ctx->session_accepted && publishing_ctx->request_pending);
    MdNostrDmInfo stale = {.rumor_created_at = req.issued_at - 3600, .live = true};
    on_session_dm(host_pk, answer, &stale, publishing_ctx);
    MD_CHECK(!publishing_ctx->session_accepted && !publishing_ctx->connection_status);
    char saved = reject.request_id[0];
    reject.request_id[0] = saved == 'a' ? 'b' : 'a';
    char *other = md_session_reject_to_json(&reject);
    on_session_dm(host_pk, other, &info, publishing_ctx);
    MD_CHECK(!publishing_ctx->session_accepted && !publishing_ctx->connection_status);
    free(other);
    on_session_dm(host_pk, answer, &info, publishing_ctx);
    MD_CHECK(publishing_ctx->session_accepted == -1 && !publishing_ctx->request_pending);
    const char *status = publishing_ctx->connection_status;
    on_session_dm(host_pk, answer, &info, publishing_ctx); /* duplicate is inert */
    MD_CHECK(publishing_ctx->connection_status == status);
    free(answer);
    return 0;
}

#ifndef _WIN32
static void *terminate_pending(void *unused) {
    (void)unused;
    void *value;
    go_channel_receive(request_sent, &value);
    kill(getpid(), SIGTERM);
    return NULL;
}
static void test_pending_shutdown(void) {
    MD_CHECK(start_signal_wake() == 0);
    request_sent = go_channel_create(1);
    ClientCtx ctx = { .session_ch = go_channel_create(1) };
    pthread_t terminator;
    MD_CHECK(pthread_create(&terminator, NULL, terminate_pending, NULL) == 0);
    MdSessionRequest out;
    int64_t start = now_us();
    MD_CHECK(negotiate_session((MdNostr *)(uintptr_t)1, &ctx, host_pk, 180000, &out) < 0);
    MD_CHECK(now_us() - start < 5000000); /* failure-only shutdown bound */
    MD_CHECK(!g_running && !ctx.request_pending && !ctx.session_accepted);
    pthread_join(terminator, NULL);
    stop_signal_wake();
    go_channel_unref(request_sent);
    request_sent = NULL;
    go_channel_unref(ctx.session_ch);
    g_running = 1;
}
#endif

int main(void) {
    const char *reasons[] = {"capture_unavailable", "request_refused"};
    for (unsigned i = 0; i < 2; i++) {
        ClientCtx ctx = {0};
        snprintf(ctx.expected_host_pk, sizeof(ctx.expected_host_pk), "%s", host_pk);
        ctx.session_ch = go_channel_create(1);
        publishing_ctx = &ctx;
        refusal_reason = reasons[i];
        MdSessionRequest out = {0};
        MD_CHECK(negotiate_session((MdNostr *)(uintptr_t)1, &ctx, host_pk, 10000, &out) < 0);
        MD_CHECK(ctx.session_accepted == -1 && !ctx.stream_token[0] && !ctx.granted_caps);
        MD_CHECK(ctx.connection_status && strstr(ctx.connection_status, "host"));
        if (!i) MD_CHECK(strstr(ctx.connection_status, "authorize screen sharing"));
        pthread_mutex_lock(&g_negotiation_mu);
        reset_session_accept_state(&ctx);
        pthread_mutex_unlock(&g_negotiation_mu);
        MD_CHECK(!ctx.connection_status && !ctx.session_accepted);
        go_channel_close(ctx.session_ch);
        go_channel_unref(ctx.session_ch);
    }
#ifndef _WIN32
    test_pending_shutdown();
#endif
    return md_test_report();
}
