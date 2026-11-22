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

const char *test_client_pubkey(const MdNostr *nostr) {
    (void)nostr;
    return "fixture-client";
}
int test_request_publish(MdNostr *nostr, const char *peer, const char *json) {
    (void)nostr;
    MD_CHECK(!strcmp(peer, host_pk));
    MdSessionRequest req;
    MD_CHECK(md_session_request_from_json(json, &req) == 0);
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
    return md_test_report();
}
