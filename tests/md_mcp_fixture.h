/*
 * metadesk — md_mcp_fixture.h
 * Shared MCP test fixture: the initialize → notifications/initialized
 * handshake and the protocol-version constant, previously copy-pasted
 * (with drift risk) into four test files.
 */
#ifndef MD_MCP_FIXTURE_H
#define MD_MCP_FIXTURE_H

#include "mcp_server.h"
#include <string.h>

#define MD_TEST_MCP_PROTOCOL "2025-03-26"

/* Drive the MCP handshake so the server reaches INITIALIZED. */
static inline void md_test_mcp_handshake(MdMcpServer *s)
{
    static const char init[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\","
        "\"id\":1,\"params\":{\"protocolVersion\":\"" MD_TEST_MCP_PROTOCOL "\","
        "\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}}}";
    md_mcp_server_handle_message(s, init, sizeof(init) - 1);

    static const char initialized[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    md_mcp_server_handle_message(s, initialized, sizeof(initialized) - 1);
}

#endif /* MD_MCP_FIXTURE_H */