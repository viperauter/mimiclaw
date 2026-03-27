# HTTP Streaming Response Design Document

## 1. Overview

This document describes the design and implementation of streaming response support for the Mimi HTTP client layer. The primary use case is supporting Server-Sent Events (SSE) protocol for MCP (Model Context Protocol) communication.

## 2. Problem Statement

### Current Architecture
The current HTTP client implementation follows a **request-response** model:
- HTTP request is sent
- Client waits for complete response
- Single callback invoked with full response body

### Limitations
This model is insufficient for streaming protocols like SSE:
- Long-lived connections must remain open
- Data arrives incrementally in chunks
- Caller needs real-time notification as data arrives
- Cannot proactively stop the stream

## 3. Design Goals

### Primary Goals
1. **Backward Compatibility** - No breaking changes to existing APIs
2. **Zero-Copy** - Direct data delivery from network buffer to application
3. **Thread Safety** - Safe stream stopping from any thread
4. **Minimal Intrusion** - Clean separation between streaming and buffered modes

### Non-Goals
- Not a general-purpose streaming framework
- Not replacing the existing callback mechanism
- Not implementing SSE protocol parsing at HTTP layer

## 4. Architecture Design

### 4.1 Core Abstraction: `mimi_http_stream_t`

```
┌─────────────────────────────────────────────────────────┐
│                   mimi_http_stream_t                    │
├─────────────────────────────────────────────────────────┤
│  on_headers(stream, resp)    - Headers received         │
│    └──► return true = continue, false = close           │
│  on_data(stream, data, len)  - Data chunk arrived       │
│    └──► return true = continue, false = close           │
│  on_close(stream, err)        - Connection closed       │
│  userdata                     - User context pointer    │
│  stop_requested               - Thread-safe stop flag   │
└─────────────────────────────────────────────────────────┘
```

### 4.2 Integration with Request Structure

```c
typedef struct {
    // ... existing fields ...
    
    /* Optional streaming handler
     * - NULL: Use buffered mode (default behavior)
     * - Set:  Enable streaming mode with this handler
     */
    mimi_http_stream_t *stream;
} mimi_http_request_t;
```

### 4.3 State Machine

```
                ┌──────────────────────────┐
                │       Send Request       │
                │  mimi_http_exec_async()  │
                └────────────┬─────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────┐
│                     MG_EV_HTTP_MSG                          │
│  Parse HTTP headers into mimi_http_response_t              │
└─────────────────────────────────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────┐
│              Check if request.stream is set                 │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ YES: Streaming mode                                │  │
│  │   • Call stream->on_headers()                     │  │
│  │   • If returns false: close connection            │  │
│  │   • Enter streaming state for MG_EV_READ         │  │
│  └──────────────────────────────────────────────────────┘  │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ NO: Buffered mode (default)                         │  │
│  │   • Accumulate full body                           │  │
│  │   • Single callback when complete                   │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────┐
│                     MG_EV_READ (Streaming)                   │
│  • Get raw data from c->recv buffer                        │
│  • Call stream->on_data(data, len)                         │
│  • If returns false or stop_requested: close connection    │
│  • Consume processed data from buffer                      │
└─────────────────────────────────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────┐
│                      MG_EV_CLOSE                            │
│  • Call stream->on_close(err) for streaming mode           │
│  • Call regular callback for buffered mode                 │
│  • Cleanup resources                                       │
└─────────────────────────────────────────────────────────────┘
```

## 5. API Reference

### 5.1 Types

```c
/* HTTP streaming handler structure
 * Defines callbacks for processing streaming responses
 */
typedef struct mimi_http_stream_s mimi_http_stream_t;

struct mimi_http_stream_s {
    /* Called when HTTP headers are received
     * Return true to continue receiving data, false to close
     */
    bool (*on_headers)(mimi_http_stream_t *stream, mimi_http_response_t *resp);
    
    /* Called for each data chunk received (zero-copy)
     * Data pointer is only valid during callback
     * Return true to continue receiving data, false to close
     */
    bool (*on_data)(mimi_http_stream_t *stream, const uint8_t *data, size_t len);
    
    /* Called when connection closes (final callback)
     * err indicates close reason (success or error)
     */
    void (*on_close)(mimi_http_stream_t *stream, mimi_err_t err);
    
    /* User data passed to all callbacks */
    void *userdata;
    
    /* Thread-safe stop flag (internal) */
    bool stop_requested;
};
```

### 5.2 Functions

```c
/* Stop an active streaming request (thread-safe)
 * Can be called from any thread, connection will be closed safely
 * in the next event loop iteration
 */
void mimi_http_stream_stop(mimi_http_stream_t *stream);
```

## 6. Implementation Plan

### Phase 1: HTTP Layer Core Implementation (http.h, http.c)
- [ ] Add `mimi_http_stream_t` type definition to http.h
- [ ] Add `stream` field to `mimi_http_request_t`
- [ ] Add streaming state fields to `http_async_req_t` internal context
- [ ] Modify `MG_EV_HTTP_MSG` handler to support streaming branch
- [ ] Add `MG_EV_READ` handler for streaming data delivery
- [ ] Implement `mimi_http_stream_stop()` function
- [ ] Modify `MG_EV_CLOSE` handler for streaming cleanup

### Phase 2: MCP SSE Integration (mcp_http_provider.c)
- [ ] Create SSE stream handler structure with embedded `mimi_http_stream_t`
- [ ] Implement SSE event parsing state machine
- [ ] Modify `mcp_http_connect()` to support streaming mode
- [ ] Add SSE message delivery to MCP message queue
- [ ] Implement proper cleanup on stream close

### Phase 3: Legacy Code Removal
- [ ] Remove `sse_thread_worker()` and thread-based implementation
- [ ] Remove redundant SSE parsing code from synchronous path
- [ ] Update configuration to use streaming by default for SSE mode

### Phase 4: Testing & Validation
- [ ] Verify backward compatibility with existing non-streaming requests
- [ ] Test SSE connection stability with MCP servers
- [ ] Test thread-safe stop functionality
- [ ] Memory leak detection with valgrind
- [ ] Performance benchmarking

## 7. Usage Example (MCP SSE)

```c
/* SSE-specific stream handler */
typedef struct {
    mimi_http_stream_t base;
    mcp_server_t *server;
    
    /* SSE protocol parsing state */
    enum {
        SSE_STATE_IDLE,
        SSE_STATE_EXPECT_DATA,
        SSE_STATE_COLLECT,
    } state;
    
    char event_buf[4096];
    size_t event_pos;
} mcp_sse_stream_t;

/* SSE Headers Callback */
static bool sse_on_headers(mimi_http_stream_t *s, mimi_http_response_t *resp) {
    mcp_sse_stream_t *sse = (mcp_sse_stream_t *)s;
    MIMI_LOGI("mcp", "SSE connected, status: %d", resp->status);
    return true;
}

/* SSE Data Callback */
static bool sse_on_data(mimi_http_stream_t *s, const uint8_t *data, size_t len) {
    mcp_sse_stream_t *sse = (mcp_sse_stream_t *)s;
    
    /* Parse SSE protocol: look for "data: ...\n\n" patterns */
    for (size_t i = 0; i < len; i++) {
        char ch = (char)data[i];
        /* ... SSE parsing logic ... */
        
        if (/* complete event found */) {
            mcp_server_deliver_message(sse->server, sse->event_buf);
        }
    }
    return true;
}

/* SSE Close Callback */
static void sse_on_close(mimi_http_stream_t *s, mimi_err_t err) {
    mcp_sse_stream_t *sse = (mcp_sse_stream_t *)s;
    MIMI_LOGI("mcp", "SSE closed: %s", mimi_err_to_name(err));
    free(sse);
}

/* Establish SSE Connection */
mimi_err_t mcp_sse_connect(mcp_server_t *server) {
    mcp_sse_stream_t *sse = calloc(1, sizeof(mcp_sse_stream_t));
    
    sse->base.on_headers = sse_on_headers;
    sse->base.on_data = sse_on_data;
    sse->base.on_close = sse_on_close;
    sse->base.userdata = server;
    sse->server = server;
    
    mimi_http_request_t req = {
        .method = "GET",
        .url = server->url,
        .headers = "Accept: text/event-stream\r\n",
        .stream = &sse->base,  /* Enable streaming mode */
    };
    
    mimi_http_response_t resp = {0};
    return mimi_http_exec_async(&req, &resp, NULL, NULL);
}
```

## 8. Design Decisions Rationale

### 8.1 Why Separate `stream` Pointer Instead of Callback Flags?
- **Clear intent**: NULL vs non-NULL clearly indicates mode
- **Zero cost when disabled**: No overhead when not using streaming
- **Extensible**: Easy to add more streaming callbacks in future

### 8.2 Why `on_headers` / `on_data` / `on_close` Naming?
- Symmetric with HTTP response lifecycle
- Familiar to developers (like DOM events)
- Clearly distinguishes from single-shot `callback`

### 8.3 Thread-Safe Stop Mechanism
- Using a simple boolean flag is the simplest thread-safe approach
- Mongoose's event-driven nature ensures safe shutdown in event loop context
- No need for complex synchronization primitives

## 9. Future Enhancements

### Potential Improvements
1. **Backpressure Support**: Add ability to pause/resume stream
2. **Chunk Encoding**: Support for chunked transfer encoding parsing
3. **Timeout Control**: Per-chunk timeout vs total request timeout
4. **Statistics**: Bytes received, chunk count, etc.

### Integration Points
- WebSocket support could reuse this infrastructure (with different parsing)
- HTTP/2 server push could use similar callback patterns
