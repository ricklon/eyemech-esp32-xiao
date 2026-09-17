#pragma once
/*
 * eye_mcp — the MCP server behind POST /mcp. Private to eye_web.
 *
 * This file knows JSON-RPC and MCP, and nothing about esp_http_server: eye_web
 * hands it the body and the few headers the protocol cares about, and gets back
 * a status code and a body. That split is what lets tools/mcp_host_test compile
 * the protocol logic on a PC against stubbed motion calls, with no board.
 *
 * Dual-era, per MCP 2026-07-28 "Backward Compatibility":
 *   - modern (2026-07-28): stateless. Every request carries its version in
 *     params._meta and mirrors method/name into MCP-Protocol-Version, Mcp-Method
 *     and Mcp-Name headers, which must match the body. server/discover.
 *   - legacy (2025-03-26 .. 2025-11-25): an initialize handshake. Sessions are
 *     optional in those revisions and this server never mints one, so it is
 *     stateless either way.
 * Neither era gets an SSE stream; every answer is a single application/json body.
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NULL for a header that was not sent. */
typedef struct {
    const char *protocol_version;  /* MCP-Protocol-Version */
    const char *method;            /* Mcp-Method           */
    const char *name;              /* Mcp-Name             */
    const char *origin;            /* Origin               */
    const char *host;              /* Host                 */
} eye_mcp_headers_t;

typedef struct {
    int   status;   /* HTTP status: 200, 202, 400, 403, 404                  */
    char *body;     /* malloc'd JSON, free() it; NULL means send no body      */
} eye_mcp_reply_t;

void eye_mcp_handle(const char *body, size_t len, const eye_mcp_headers_t *hdr,
                    eye_mcp_reply_t *out);

#ifdef __cplusplus
}
#endif
