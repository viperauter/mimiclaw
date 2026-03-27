# DashScope MCP Implementation vs Official Specification

This document compares DashScope MCP server implementation against the official MCP specification (2025-11-25).

## Overview

DashScope MCP server implements a variant of the Streamable HTTP transport with some deviations from the official specification. This document analyzes these differences and their implications.

## Official Specification Requirements

### Streamable HTTP Transport (from mcp_transports.md)

The official specification requires:

1. **POST Request Handling**:
   - Server MUST return either `Content-Type: text/event-stream` to initiate SSE stream, OR `Content-Type: application/json` to return one JSON object
   - Client MUST support both cases

2. **SSE Stream Behavior**:
   - Server SHOULD immediately send an SSE event with event ID and empty `data` field
   - Server MAY close connection at any time to avoid holding long-lived connection
   - Client SHOULD then "poll" SSE stream by attempting to reconnect
   - If server closes connection before terminating stream, it SHOULD send a `retry` field
   - Client MUST respect `retry` field
   - After JSON-RPC response is sent, server SHOULD terminate SSE stream

3. **Session Management**:
   - Server MAY assign session ID during initialization via `MCP-Session-Id` header
   - Client MUST include `MCP-Session-Id` header on all subsequent requests
   - Server MAY terminate session at any time (HTTP 404)

4. **Resumability**:
   - Server MAY attach `id` field to SSE events
   - Client SHOULD use `Last-Event-ID` header to resume after disconnection
   - Server MAY replay messages after last event ID

## DashScope Implementation

### Observed Behavior

Based on our debugging and testing:

1. **Connection Lifecycle**:
   ```
   1. Client connects to SSE endpoint
      GET https://dashscope.aliyuncs.com/api/v1/mcps/WebParser/sse
   
   2. Server sends endpoint event
      event: endpoint
      data: /api/v1/mcps/WebParser/message?sessionId=<uuid>
   
   3. Server closes SSE connection immediately
      Connection closed after endpoint event is sent
   
   4. Client sends POST requests to message URL
      POST https://dashscope.aliyuncs.com/api/v1/mcps/WebParser/message?sessionId=<uuid>
      Headers: MCP-Session-Id: <uuid>
   
   5. Server returns empty 200 responses
      HTTP/1.1 200 OK
      Content-Length: 0
   ```

2. **Rate Limiting**:
   - Server enforces rate limiting (HTTP 429 responses)
   - No explicit `retry` field in responses
   - Client must implement exponential backoff

3. **Session Management**:
   - SessionId is provided in endpoint event (as query parameter)
   - SessionId must be included in POST request headers
   - Session can expire (HTTP 404)

## Compliance Analysis

### ✅ Compliant Aspects

| Requirement | DashScope Implementation | Notes |
|-------------|------------------------|---------|
| **Server MAY close connection** | ✅ YES | Closes SSE connection after endpoint discovery |
| **Session ID assignment** | ✅ YES | Provides sessionId in endpoint event |
| **Client MUST include session ID** | ✅ YES | Requires MCP-Session-Id header |
| **Server MAY terminate session** | ✅ YES | Returns HTTP 404 for expired sessions |
| **Rate limiting enforcement** | ✅ YES | Returns HTTP 429 |
| **Client SHOULD support both response types** | ✅ YES | Client handles both SSE and JSON responses |

### ❌ Non-Compliant Aspects

| Requirement | DashScope Implementation | Specification | Impact |
|-------------|------------------------|--------------|---------|
| **POST response format** | Returns empty 200 response | MUST return SSE stream OR JSON object | Client cannot receive actual response data |
| **SSE event with empty data** | Not sent | SHOULD send immediately after connection | Client doesn't get initial event ID |
| **Retry field in SSE** | Not provided | SHOULD send before closing connection | Client doesn't know when to reconnect |
| **Last-Event-ID support** | Not supported | Client SHOULD use for resumption | Cannot resume broken connections |
| **SSE stream termination** | Connection closed immediately | SHOULD terminate after sending response | No SSE stream for responses |
| **Response via SSE** | Responses via POST (empty) | SHOULD be in SSE stream | Requires special handling |

## Detailed Analysis

### Issue 1: Empty POST Responses

**Specification**:
> If input is a JSON-RPC *request*, server MUST either return `Content-Type: text/event-stream`, to initiate an SSE stream, or `Content-Type: application/json`, to return one JSON object.

**DashScope Behavior**:
- Returns `HTTP/1.1 200 OK` with `Content-Length: 0`
- No response body
- No SSE stream initiated

**Impact**:
- Client cannot receive actual response data from server
- Requires special handling to treat empty responses as success
- Makes it impossible to receive tool lists, execution results, etc. via POST

**Workaround in MimiClaw**:
```c
// Handle empty 200 responses for DashScope
if (hresp.status >= 200 && hresp.status < 300 && is_empty_response) {
    if (strstr(request_json, "\"method\":\"initialize\"")) {
        return strdup("{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":{}}" );
    }
    if (strstr(request_json, "\"method\":\"tools/list\"")) {
        return strdup("{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":{\"tools\":[]}}" );
    }
}
```

### Issue 2: No SSE Stream for Responses

**Specification**:
> The SSE stream SHOULD eventually include a JSON-RPC *response* for the JSON-RPC *request* sent in POST body.
> After JSON-RPC *response* has been sent, server SHOULD terminate SSE stream.

**DashScope Behavior**:
- SSE connection is closed immediately after endpoint discovery
- No SSE stream is maintained for responses
- All responses come via POST (which are empty)

**Impact**:
- Cannot receive server-to-client notifications
- Cannot receive streaming responses
- Requires polling mechanism for responses (not implemented)

### Issue 3: Missing Retry Field

**Specification**:
> If server does close *connection* prior to terminating *SSE stream*, it SHOULD send an SSE event with a standard `retry` field before closing the connection. The client MUST respect `retry` field, waiting the given number of milliseconds before attempting to reconnect.

**DashScope Behavior**:
- No `retry` field in SSE events
- Connection closed without retry guidance

**Impact**:
- Client doesn't know when to reconnect
- Must implement arbitrary retry logic
- May cause excessive reconnection attempts

### Issue 4: No Resumability Support

**Specification**:
> Servers MAY attach an `id` field to their SSE events... If client wishes to resume after a disconnection, it SHOULD issue an HTTP GET to MCP endpoint, and include `Last-Event-ID` header to indicate last event ID it received.

**DashScope Behavior**:
- No event IDs in SSE events
- No `Last-Event-ID` header support
- Cannot resume after disconnection

**Impact**:
- Message loss on disconnection
- Cannot resume from last known state
- Must start new session on every reconnection

### Issue 5: Backwards Compatibility with Old HTTP+SSE

**Specification** (Backwards Compatibility section):
> Clients wanting to support older servers should:
> 1. Accept an MCP server URL from user, which may point to either a server using old transport or new transport.
> 2. Attempt to POST an `InitializeRequest` to server URL...
> 3. If it fails with HTTP 400, 404, or 405:
>    - Issue a GET request to server URL, expecting that this will open an SSE stream and return an `endpoint` event as first event.

**DashScope Behavior**:
- Uses `endpoint` event pattern (old HTTP+SSE transport)
- Requires GET to SSE endpoint first
- Returns endpoint event with message URL

**Impact**:
- DashScope appears to implement the deprecated HTTP+SSE transport (2024-11-05)
- Not the new Streamable HTTP transport (2025-11-25)
- Requires backwards compatibility handling in clients

## Conclusion

### Overall Assessment

DashScope MCP server implementation is **partially compliant** with the official MCP specification (2025-11-25):

**Strengths**:
- ✅ Implements basic session management
- ✅ Supports rate limiting
- ✅ Provides endpoint discovery mechanism
- ✅ Works with standard HTTP headers

**Weaknesses**:
- ❌ Does not follow POST response format requirements
- ❌ Does not maintain SSE stream for responses
- ❌ Lacks retry mechanism
- ❌ No resumability support
- ❌ Appears to use deprecated transport version

### Recommendation for Clients

When implementing MCP client for DashScope:

1. **Handle Empty Responses**: Treat empty 200 responses as success
2. **Implement Retry Logic**: Use exponential backoff for rate limiting
3. **Session Management**: Include sessionId in all POST requests
4. **Backwards Compatibility**: Support both new and old transport patterns
5. **Error Handling**: Gracefully handle connection drops and 429 responses

### Recommendation for DashScope

To achieve full compliance with MCP specification (2025-11-25):

1. **Return Actual Responses**: Include JSON-RPC response in POST response body or SSE stream
2. **Maintain SSE Stream**: Keep SSE connection open for responses and notifications
3. **Add Retry Field**: Include `retry` field before closing connections
4. **Support Resumability**: Add event IDs and support `Last-Event-ID` header
5. **Upgrade to New Transport**: Implement Streamable HTTP transport (2025-11-25)

## References

- [MCP Specification (2025-11-25)](https://spec.modelcontextprotocol.io/)
- [MCP Transports Documentation](./mcp_transports.md)
- [DashScope MCP Documentation](https://help.aliyun.com/zh/model-context-protocol/)
