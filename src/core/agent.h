/*
 * metadesk — agent.h
 * Agent action handler: receive actions, resolve targets, inject, report deltas.
 *
 * This module orchestrates the agent interaction loop on the host side:
 *   1. Parse incoming MD_PKT_ACTION JSON
 *   2. Resolve target_id to screen coordinates via accessibility tree
 *   3. Inject the action via input injection backend
 *   4. Wait briefly for the UI to settle
 *   5. Walk the accessibility tree for deltas
 *   6. Send MD_PKT_UI_TREE or MD_PKT_UI_TREE_DELTA back to client
 *
 * See spec §10.2 for the interaction loop and §3.2 for action format.
 */
#ifndef MD_AGENT_H
#define MD_AGENT_H

#include "action.h"
#include "a11y.h"
#include "input.h"
#include "stream.h"
#include "packet.h"
#include "session.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque agent handler context */
typedef struct MdAgent MdAgent;

/* Agent configuration */
typedef struct {
    MdA11yCtx    *a11y;         /* shared accessibility tree context    */
    MdInput      *input;        /* shared input injection context       */
    MdTreeFormat  tree_format;  /* negotiated tree format for client   */
    uint32_t      settle_ms;    /* ms to wait after injection (default: 100) */
} MdAgentConfig;

/* Default settle time after action injection */
#define MD_AGENT_DEFAULT_SETTLE_MS 100

/* Create agent handler. Takes shared references (does NOT own them). */
MdAgent *md_agent_create(const MdAgentConfig *cfg);

/* Handle an incoming MD_PKT_ACTION packet.
 * Parses the JSON, resolves target, injects action, computes delta,
 * and sends the response packet(s) on the stream.
 *
 * stream: the client connection to send responses on
 * seq: pointer to the packet sequence counter (incremented per send)
 * payload: the raw JSON action payload
 * payload_len: byte length
 *
 * Returns 0 on success, -1 on error. */
int md_agent_handle_action(MdAgent *agent, MdStream *stream,
                           uint32_t *seq,
                           const uint8_t *payload, uint32_t payload_len);

/* Send a full UI tree snapshot to the client.
 * Called on initial connection or when client requests a full refresh. */
int md_agent_send_tree(MdAgent *agent, MdStream *stream, uint32_t *seq);

/* Send a UI tree delta to the client.
 * Computes diff against the previous snapshot. */
int md_agent_send_delta(MdAgent *agent, MdStream *stream, uint32_t *seq);

/* Get the number of actions handled. */
uint32_t md_agent_get_action_count(const MdAgent *agent);

/* Destroy agent handler. Does NOT destroy shared atspi/input contexts. */
void md_agent_destroy(MdAgent *agent);

#ifdef __cplusplus
}
#endif

#endif /* MD_AGENT_H */
