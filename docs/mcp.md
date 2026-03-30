# Model Context Protocol (MCP) Implementation

This document describes the MCP (Model Context Protocol) implementation in the mimiclaw project, including its architecture, configuration, and usage.

## Overview

MCP is an open protocol that enables seamless integration between LLM applications and external data sources and tools. The mimiclaw implementation provides a standardized way to connect LLMs with the context they need, supporting multiple transport methods: stdio, HTTP, SSE, and streamable-http.

The offical MCP protocol reference doc/mcp_transports.md

## Debugging and Troubleshooting

For detailed information on debugging MCP implementation issues, including comparing with the Python reference implementation, see the [MCP Debugging Guide](mcp_debug.md).

### Common Issues

#### 1. SSE Long-Lived Connection Issues

**Symptoms**:
- First request (initialize) works
- Subsequent requests fail
- Connection drops after initial response

**Debugging Steps**:
1. Verify Python implementation works correctly
2. Compare network traffic between implementations
3. Check session ID extraction and usage
4. Ensure keep-alive events are properly handled

See the [MCP Debugging Guide](mcp_debug.md) for detailed troubleshooting steps.

#### 2. Transport-Specific Issues

**stdio Transport**:
- Check if the child process is running
- Verify pipe connections
- Check stderr for error messages

**HTTP/SSE Transport**:
- Verify server URL is correct
- Check network connectivity
- Verify headers (Authorization, etc.)
- Check for CORS issues

#### 3. Tool Discovery Issues

- Check MCP server initialization logs
- Verify tool list is being cached
- Check for JSON parsing errors

### Logging

Enable debug logging for MCP:

```bash
# Run with MCP debug logging
build/mimiclaw -f logs/$(date +%Y%m%d_%H%M%S).log
```

## Core Components

The MCP implementation consists of the following key components:

| Component | File | Description |
|-----------|------|-------------|
| Main Provider | `mcp_provider.c/.h` | Main provider implementation that manages all MCP servers |
| Core Protocol | `mcp_provider_core.c/.h` | Core protocol implementation, tool management, initialization |
| HTTP Transport | `mcp_http_provider.c` | HTTP/SSE/streamable-http transport implementation |
| stdio Transport | `mcp_stdio_provider.c` | stdio transport implementation for process-to-process communication |
| Internal | `mcp_provider_internal.h` | Internal data structures and function declarations |
| Refresh Tool | `tool_mcp_refresh.c/.h` | MCP tool refresh functionality |
| CLI Command | `cmd_mcp_refresh.c` | CLI command for refreshing MCP tools |

## Directory Structure

```
main/
  ├── agent/tools/providers/
  │   ├── mcp_provider.c          # Main MCP provider implementation
  │   ├── mcp_provider.h          # Provider header file
  │   ├── mcp_provider_core.c     # Core protocol implementation
  │   ├── mcp_provider_core.h     # Core protocol header
  │   ├── mcp_provider_internal.h # Internal data structures
  │   ├── mcp_http_provider.c     # HTTP transport implementation
  │   ├── mcp_stdio_provider.c   # stdio transport implementation
  │   ├── mcp.md                  # MCP protocol documentation
  │   └── mcp_transports.md        # Transport layer documentation
  ├── agent/tools/
  │   ├── tool_mcp_refresh.c      # MCP tool refresh functionality
  │   └── tool_mcp_refresh.h      # MCP tool refresh header
  └── interface/commands/
      └── cmd_mcp_refresh.c      # CLI command for MCP refresh
```

## Configuration

MCP servers are configured in the application's configuration file under the `tools` section. Here's an example configuration:

```json
{
  "tools": {
    "mcpServers": [
      {
        "enabled": true,
        "name": "example_stdio",
        "type": "stdio",
        "command": "/path/to/mcp/server",
        "args": "--debug",
        "requires_confirmation": true
      },
      {
        "type": "http",
        "url": "http://localhost:8000",
        "requires_confirmation": false
      },
      {
        "name": "example_sse",
        "type": "sse",
        "url": "http://localhost:8000/sse",
        "requires_confirmation": false
      },
      {
        "name": "example_streamable",
        "type": "streamable-http",
        "url": "http://localhost:8000/mcp",
        "requires_confirmation": false
      },
      {
        "name": "example_with_headers",
        "type": "http",
        "url": "http://localhost:8000",
        "headers": {
          "Authorization": "Bearer token123",
          "X-API-Key": "secret"
        },
        "requires_confirmation": false
      },
      {
        "enabled": false,
        "name": "paused_server",
        "type": "sse",
        "url": "http://localhost:9999/sse"
      }
    ]
  }
}
```

Omitting `enabled` is equivalent to `"enabled": true`. When `enabled` is `false`, that entry is skipped: no discovery, and its tools are not merged into the model tool list.

`name` is optional. If empty or omitted, the runtime assigns a stable display name (for example from the URL host, the stdio command basename, or `mcp_server_<index>`), and ensures it does not collide with other configured servers.

`requires_confirmation` defaults to `true` if omitted. When `true`, all tools from this server require user confirmation before execution.

### Configuration Parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `enabled` | boolean | Optional. Defaults to `true` if omitted. If `false`, this server is not loaded and its tools are not exposed to the LLM. |
| `name` | string | Optional. Human-readable id used in tool names (`mcp::<name>::<tool>`). If omitted, a name is derived from `url` or `command` (see above). |
| `type` | string | Transport type: "stdio", "http", "sse", "streamable-http" |
| `command` | string | Command to run for stdio transport |
| `args` | string | Additional arguments for stdio command (space-separated) |
| `url` | string | URL for HTTP/SSE/streamable-http transport |
| `headers` | object | HTTP headers for HTTP/SSE/streamable-http transport |
| `requires_confirmation` | boolean | Optional. Defaults to `true`. Whether this tool requires user confirmation before execution |

## Transport Methods

The MCP implementation supports four transport methods:

### 1. stdio Transport (`type: "stdio"`)

- Uses pipe-based communication between processes
- Spawns child processes for each MCP server
- Provides bi-directional communication through stdin/stdout
- Captures and forwards stderr to logs (avoids polluting CLI output)

### 2. HTTP Transport (`type: "http"`)

- Auto-detects between streamable-http and legacy SSE modes
- Falls back automatically if detection fails
- Supports standard HTTP POST requests for JSON-RPC messages

### 3. SSE Transport (`type: "sse"`)

- Forces legacy HTTP+SSE mode
- Uses SSE (Server-Sent Events) for asynchronous notifications
- Maintains session state through session IDs
- Discovers message endpoint via `endpoint` event

### 4. Streamable HTTP Transport (`type: "streamable-http"`)

- Forces modern streamable HTTP mode
- Single MCP endpoint supports POST+GET
- More efficient than legacy SSE mode

## Implementation Details

### MCP Server State Machine

The MCP server state machine manages the lifecycle of an MCP server connection:

```
UNINITIALIZED → INITIALIZING → INITIALIZED → ERROR
```

### HTTP Mode Detection

For HTTP transport, the implementation automatically detects the optimal mode:

1. First attempts streamable HTTP mode
2. If fails, falls back to legacy SSE mode
3. Uses the first working mode for subsequent requests

### SSE Streaming

For SSE transport, the implementation:

1. Establishes SSE connection
2. Waits for `endpoint` event with message URL
3. Extracts session ID from endpoint URL
4. Maintains long-lived connection
5. Handles keep-alive events
6. Sends messages via POST to message endpoint

### stdio Process Management

For stdio transport, the implementation:

1. Forks child process
2. Sets up pipe communication
3. Captures stderr for logging
4. Manages process lifecycle
5. Restarts on failure

## Core Data Structures

### MCP Server Structure

```c
typedef struct mcp_server_s {
    char name[64];             // Server name
    char url[256];              // URL (HTTP/SSE/streamable-http)
    char command[256];            // Command (stdio)
    char args[256];               // Arguments (stdio)
    bool requires_confirmation;  // Whether tool requires confirmation
    
    // HTTP headers
    char extra_http_headers[1024]; // CRLF-separated header block
    
    // stdio-specific
    pid_t pid;                  // Process ID
    int to_child;               // Parent -> child pipe (stdin)
    int from_child;             // Child -> parent pipe (stdout)
    int err_from_child;         // Child -> parent pipe (stderr)
    bool started;               // Whether started
    
    // Session management
    bool initialized;           // Whether initialized
    char negotiated_protocol_version[32]; // Protocol version
    mcp_transport_type_t transport_type; // Configured transport type
    mcp_http_mode_t http_mode;           // Current HTTP mode (runtime detected)
    char session_id[192];       // Session ID
    char last_event_id[128];    // Last event ID (SSE)
    char sse_message_url[512]; // SSE message URL
    int sse_retry_ms;           // SSE retry interval
    long long last_ping_ms;     // Last ping time
    
    // Streaming SSE connection (event-driven)
    mcp_sse_stream_t *sse_stream;        // Active SSE stream (NULL if not connected)
    mcp_stream_ready_cb_t stream_ready_cb; // Callback when stream is ready
    mimi_err_t stream_connect_error;     // Last stream connection error

    char *tools_json;           // Cached tools JSON
    
    // Stderr accumulation (for forwarding to logs)
    char stderr_accum[2048];
    size_t stderr_accum_len;
} mcp_server_t;
```

### Core Functions

| Function | Description |
|----------|-------------|
| `mcp_provider_get()` | Returns the MCP tool provider instance |
| `mcp_provider_request_refresh()` | Requests background refresh of MCP servers |
| `mcp_core_initialize()` | Initializes an MCP server |
| `mcp_tools_to_openai_json()` | Converts MCP tools to OpenAI format |
| `mcp_find_server_by_tool()` | Finds server by tool name |
| `mcp_rebuild_merged_tools_json()` | Rebuilds merged tools JSON |
| `mcp_http_exchange()` | Performs HTTP JSON-RPC exchange |
| `mcp_sse_stream_connect()` | Establishes SSE streaming connection |
| `mcp_sse_stream_disconnect()` | Closes SSE streaming connection |
| `mcp_stdio_start()` | Starts stdio MCP server |
| `mcp_stdio_exchange()` | Performs stdio JSON-RPC exchange |

## Error Handling

The MCP implementation includes comprehensive error handling:

- **Invalid arguments**: Returns `MIMI_ERR_INVALID_ARG`
- **Server not found**: Returns `MIMI_ERR_NOT_FOUND`
- **Execution failure**: Returns `MIMI_ERR_FAIL`
- **Network errors**: Returns appropriate HTTP error codes
- **Process errors**: Handles stdio server crashes and restarts
- **Timeout errors**: Handles request timeouts gracefully

## Performance Considerations

- **Connection Pooling**: HTTP connections are reused where possible
- **Tool Caching**: Tool lists are cached to reduce network overhead
- **Ping Mechanism**: Periodic pings maintain connection health
- **Timeout Handling**: Requests have configurable timeouts
- **Stream Reuse**: SSE streams are maintained for efficiency
- **Mutex Protection**: Thread-safe access to shared resources

## Testing

The MCP implementation includes integration tests:

- `test_mcp_http_server.py`: Tests HTTP transport
- `test_mcp_server.py`: Tests stdio transport
- `test_mcp_integration.py`: Tests end-to-end functionality

## Command Line Interface

MCP provides a CLI command for managing MCP tools:

| Command | Description |
|---------|-------------|
| `/mcp_refresh` | Refresh MCP tools from all configured servers |

## Architecture Benefits

- **Modular Design**: Clear separation between transport types
- **Extensible**: Easy to add new transport types
- **Backward Compatible**: Supports both legacy and modern MCP modes
- **Robust**: Comprehensive error handling and recovery
- **Efficient**: Caching and connection reuse for optimal performance

## Conclusion

The MCP implementation in mimiclaw provides a robust, flexible way to integrate external tools and data sources with LLMs. By supporting multiple transport methods (stdio, HTTP, SSE, streamable-http), it can adapt to a wide range of use cases, from local tool integration to remote service connections.

With its standardized protocol, comprehensive error handling, and runtime refresh capabilities, MCP enables seamless integration between LLMs and the tools they need to perform complex tasks, opening up new possibilities for AI-powered applications.

For debugging and troubleshooting guidance, see the [MCP Debugging Guide](mcp_debug.md).
