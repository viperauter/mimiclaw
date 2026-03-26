#pragma once

#include "mimi_err.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <stddef.h>

typedef enum {
    MCP_HTTP_MODE_UNKNOWN = 0,
    MCP_HTTP_MODE_STREAMABLE = 1, /* Streamable HTTP: single MCP endpoint supports POST+GET */
    MCP_HTTP_MODE_LEGACY_HTTP_SSE = 2, /* Deprecated HTTP+SSE: SSE endpoint returns `endpoint` event */
} mcp_http_mode_t;

/* HTTP transport mode override for configuration.
 * Allows forcing a specific transport mode instead of auto-detection.
 * Values match MCP standard "type" field: stdio, sse, streamable-http.
 */
typedef enum {
    MCP_TRANSPORT_UNKNOWN = 0,
    MCP_TRANSPORT_STDIO = 1,      /* type: "stdio" */
    MCP_TRANSPORT_SSE = 2,        /* type: "sse" (legacy HTTP+SSE) */
    MCP_TRANSPORT_STREAMABLE_HTTP = 3, /* type: "streamable-http" */
    MCP_TRANSPORT_HTTP = 4,       /* type: "http" (auto-detect) */
} mcp_transport_type_t;

typedef struct {
    bool use_http;
    char name[64];
    char command[256];
    char args[512];  /* additional arguments, space-separated */
    char url[512];
    bool requires_confirmation;

    /* Optional extra HTTP headers for MCP HTTP/SSE transport.
     * Stored as a single CRLF-separated header block, e.g.:
     *   Authorization: Bearer xxx\r\nX-Foo: bar\r\n
     */
    char extra_http_headers[1024];

    pid_t pid;
    int to_child;   /* parent writes -> child stdin */
    int from_child; /* parent reads  <- child stdout */
    int err_from_child; /* parent reads <- child stderr */
    bool started;

    bool initialized;
    char negotiated_protocol_version[32];
    mcp_transport_type_t transport_type; /* Configured transport type from "type" field */
    mcp_http_mode_t http_mode;           /* Current HTTP mode (runtime detected) */

    char session_id[192];
    char last_event_id[128];
    char sse_message_url[512];
    int sse_retry_ms;
    long long last_ping_ms;

    char *tools_json;

    /* Stderr is not part of MCP protocol; we capture and forward it to logs.
     * This avoids polluting interactive CLI output with progress spinners.
     */
    char stderr_accum[2048];
    size_t stderr_accum_len;
} mcp_server_t;

typedef void (*mcp_server_msg_cb_t)(mcp_server_t *s, cJSON *msg);

char *mcp_http_exchange(mcp_server_t *s, const char *id_str, const char *request_json,
                        mcp_server_msg_cb_t on_notification, mcp_server_msg_cb_t on_request);
mimi_err_t mcp_http_notify_post(mcp_server_t *s, const char *request_json);

mimi_err_t mcp_stdio_start(mcp_server_t *s);
char *mcp_stdio_exchange(mcp_server_t *s, uint64_t *rpc_id_next, const char *method, cJSON *params,
                         mcp_server_msg_cb_t on_notification, mcp_server_msg_cb_t on_request);
mimi_err_t mcp_stdio_notify(mcp_server_t *s, const char *method, cJSON *params);
mimi_err_t mcp_stdio_send_json(mcp_server_t *s, cJSON *obj);
