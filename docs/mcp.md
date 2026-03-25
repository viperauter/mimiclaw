# Model Context Protocol (MCP) Implementation

This document describes the MCP (Model Context Protocol) implementation in the mimiclaw project, including its architecture, configuration, and usage.

## Overview

MCP is an open protocol that enables seamless integration between LLM applications and external data sources and tools. The mimiclaw implementation provides a standardized way to connect LLMs with the context they need, supporting both HTTP and stdio transport methods.

## Core Components

The MCP implementation consists of the following key components:

| Component | File | Description |
|-----------|------|-------------|
| Core | `mcp_provider_core.c/.h` | Core protocol implementation, including tool management, initialization, and ping mechanism |
| HTTP Transport | `mcp_http_provider.c` | HTTP transport implementation, including SSE (Server-Sent Events) support |
| stdio Transport | `mcp_stdio_transport.c` | stdio transport implementation for process-to-process communication |
| Provider | `mcp_provider.c/.h` | Main provider implementation that manages all MCP servers |
| Internal | `mcp_provider_internal.h` | Internal data structures and function declarations |

## Directory Structure

```
main/agent/tools/providers/
  ├── mcp_provider.c          # Main MCP provider implementation
  ├── mcp_provider.h          # Provider header file
  ├── mcp_provider_core.c     # Core protocol implementation
  ├── mcp_provider_core.h     # Core protocol header
  ├── mcp_provider_internal.h # Internal data structures
  ├── mcp_http_provider.c     # HTTP transport implementation
  ├── mcp_stdio_transport.c   # stdio transport implementation
  ├── mcp.md                  # MCP protocol documentation
  └── mcp_transports.md        # Transport layer documentation
```

## Configuration

MCP servers are configured in the application's configuration file under the `providers` section. Here's an example configuration:

```json
{
  "tools": {
    "mcpServers": [
      {
        "name": "example_stdio",
        "transport": "stdio",
        "command": "/path/to/mcp/server",
        "requires_confirmation": true
      },
      {
        "name": "example_http",
        "transport": "http",
        "url": "http://localhost:8000",
        "requires_confirmation": false
      }
    ]
  }
}
```

### Configuration Parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `name` | string | Unique name for the MCP server |
| `transport` | string | Transport type: "stdio" or "http" |
| `command` | string | Command to run for stdio transport |
| `url` | string | URL for HTTP transport |
| `requires_confirmation` | boolean | Whether tool execution requires user confirmation |

## Transport Methods

The MCP implementation supports two transport methods:

### 1. HTTP Transport

- Uses standard HTTP POST requests for JSON-RPC messages
- Supports SSE (Server-Sent Events) for asynchronous notifications
- Maintains session state through session IDs

### 2. stdio Transport

- Uses pipe-based communication between processes
- Spawns child processes for each MCP server
- Provides bi-directional communication through stdin/stdout

## Workflow

### Initialization

1. The MCP provider initializes during application startup
2. It reads the configuration and creates server instances
3. For each server, it establishes the connection (spawns process for stdio, or prepares HTTP client)
4. It sends an `initialize` request to each server to negotiate capabilities

### Tool Discovery

1. The provider sends a `tools/list` request to each server
2. Servers return their available tools
3. The provider merges all tools into a single list
4. Tools are exposed to the LLM in OpenAI-compatible format

### Tool Execution

1. The LLM requests to execute an MCP tool
2. The provider identifies the target server based on the tool name
3. It sends a `tools/call` request to the server with the tool arguments
4. The server executes the tool and returns the result
5. The provider forwards the result back to the LLM

## Security Considerations

- **User Consent**: Tools can be configured to require user confirmation before execution
- **Session Management**: HTTP transport uses session IDs to maintain state
- **Error Handling**: Robust error handling ensures system stability
- **Input Validation**: All inputs are validated to prevent malicious requests

## Tool Naming Convention

MCP tools are exposed to the LLM with the following naming convention:

```
mcp::server_name::tool_name
```

Where:
- `server_name` is the name configured for the MCP server
- `tool_name` is the name of the tool as defined by the server

## Example Usage

### Configuration Example

```json
{
  "tools": {
    "mcpServers": [
      {
        "name": "calculator",
        "transport": "stdio",
        "command": "./calculator_server",
        "requires_confirmation": false
      }
    ]
  }
}
```

### Tool Execution Example

When the LLM requests to execute a tool:

```json
{
  "toolcall": {
    "thought": "I need to calculate 2 + 2",
    "name": "mcp::calculator::add",
    "params": {
      "a": 2,
      "b": 2
    }
  }
}
```

The MCP provider will:
1. Extract the server name "calculator" and tool name "add"
2. Send the request to the calculator server
3. Return the result to the LLM

## Key Interfaces

### Provider Registration

```c
// Register the MCP provider
(void)tool_provider_register(mcp_provider_get());
```

### Tool Execution

```c
// Execute an MCP tool
mimi_err_t err = tool_registry_execute("mcp::server::tool", input_json, output, output_size, session_ctx);
```

## Implementation Details

### Server Structure

```c
typedef struct {
    bool use_http;              // Use HTTP transport
    char name[64];              // Server name
    char command[256];          // stdio command
    char url[512];              // HTTP URL
    bool requires_confirmation;  // Require confirmation
    
    // stdio-specific
    pid_t pid;                  // Process ID
    int to_child;               // Parent -> child pipe
    int from_child;             // Child -> parent pipe
    bool started;               // Whether started
    
    // Session management
    bool initialized;           // Whether initialized
    char negotiated_protocol_version[32]; // Protocol version
    char session_id[192];       // Session ID
    char last_event_id[128];    // Last event ID (SSE)
    int sse_retry_ms;           // SSE retry interval
    long long last_ping_ms;     // Last ping time
    
    char *tools_json;           // Cached tools JSON
} mcp_server_t;
```

### Core Functions

| Function | Description |
|----------|-------------|
| `mcp_provider_get()` | Returns the MCP tool provider instance |
| `mcp_core_initialize()` | Initializes an MCP server |
| `mcp_core_refresh_server_tools()` | Refreshes the tool list for a server |
| `mcp_http_exchange()` | Performs HTTP JSON-RPC exchange |
| `mcp_stdio_exchange()` | Performs stdio JSON-RPC exchange |

## Error Handling

The MCP implementation includes comprehensive error handling:

- **Invalid arguments**: Returns `MIMI_ERR_INVALID_ARG`
- **Server not found**: Returns `MIMI_ERR_NOT_FOUND`
- **Execution failure**: Returns `MIMI_ERR_FAIL`
- **Network errors**: Returns appropriate HTTP error codes

## Performance Considerations

- **Connection Pooling**: HTTP connections are reused where possible
- **Tool Caching**: Tool lists are cached to reduce network overhead
- **Ping Mechanism**: Periodic pings maintain connection health
- **Timeout Handling**: Requests have configurable timeouts

## Testing

The MCP implementation includes integration tests:

- `test_mcp_http_server.py`: Tests HTTP transport
- `test_mcp_server.py`: Tests stdio transport
- `test_mcp_integration.py`: Tests end-to-end functionality

## Conclusion

The MCP implementation in mimiclaw provides a robust, flexible way to integrate external tools and data sources with LLMs. By supporting both HTTP and stdio transport methods, it can adapt to a wide range of use cases, from local tool integration to remote service connections.

With its standardized protocol and comprehensive error handling, MCP enables seamless integration between LLMs and the tools they need to perform complex tasks, opening up new possibilities for AI-powered applications.