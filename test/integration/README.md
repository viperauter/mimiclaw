# Integration Tests for MimicLaw MCP Integration

This directory contains end-to-end integration tests for the Model Context Protocol (MCP) functionality in MimicLaw.

## Test Architecture

### Overview
The integration test framework uses a **black-box testing approach** that runs the actual `mimiclaw` binary against virtualized services:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          Integration Test Environment                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────────────┐     ┌──────────────────┐     ┌──────────────────┐     │
│  │   pytest Tests   │     │  Virtual LLM     │     │   MCP Test       │     │
│  │   (test cases)   │────▶│    Server        │────▶│    Server        │     │
│  └────────┬─────────┘     │ (dynamic resp.)  │     │  (stdio mode)    │     │
│           │               └──────────────────┘     └──────────────────┘     │
│           │                        ▲                                        │
│           │                        │ HTTP calls                             │
│           ▼                        ▼                                        │
│  ┌──────────────────┐     ┌──────────────────┐                              │
│  │   mimiclaw       │────▶│  Test Config     │                              │
│  │   (binary)       │     │  Generator       │                              │
│  └──────────────────┘     └──────────────────┘                              │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Key Components

1. **Virtual LLM Server** - A mock OpenAI-compatible API server that allows dynamic configuration of responses via REST API
2. **MCP Test Server** - A test MCP server that implements various tools for integration testing
3. **pytest Framework** - Test orchestration with fixtures for environment management
4. **Test Controller** - Manages test services and configuration

## Test Files

| File | Purpose |
|------|---------|
| [`test_mcp_integration.py`](./test_mcp_integration.py) | **Main test suite** containing MCP integration tests **and built-in subagent tool tests** (15 test cases) |
| [`conftest.py`](./conftest.py) | pytest fixtures and shared configuration for test environment |
| [`virtual_llm_server.py`](./virtual_llm_server.py) | Flask-based mock LLM server with dynamic response configuration |
| [`test_mcp_server.py`](./test_mcp_server.py) | Test MCP server using **official MCP Python SDK** implementing tools like `echo`, `add`, `get_test_data`. **Supports multiple transport modes: stdio, sse, streamable-http** |
| [`test_controller.py`](./test_controller.py) | Standalone test environment manager (start/stop/configure services) |
| [`test_runner.py`](./test_runner.py) | Test execution orchestrator and environment setup |

## Test Categories

The test suite covers these scenarios:

### 1. MCP Discovery Tests
- Configuration parsing and server startup
- Tool discovery and registration
- Dynamic LLM server configuration

### 2. MCP Execution Tests
- Tool calling (`echo`, `add`, `get_test_data`)
- Response handling and error cases
- Parameter validation

### 3. Configuration Scenarios
- Multiple MCP servers configuration
- Confirmation requirement settings
- Workspace and timeout configurations

### 4. LLM Control Flow
- Multi-turn conversations
- Sequential tool calls
- Error recovery scenarios

### 5. Built-in Tool Tests
- **Subagent management** - Spawn, list, cancel, join operations
- Subagent tool restriction (tools allowlist)
- Subagent lifecycle management

### 6. End-to-End Workflows
- Complete workflow from user input to final response
- Full MCP integration lifecycle
- Subagent-based task decomposition

### 7. HTTP Transport Tests
- HTTP MCP server startup and connectivity
- Tool discovery via HTTP transport
- Tool execution via HTTP transport
- Transport mode verification

### 8. SSE Transport Tests
- SSE MCP server startup and connectivity
- Tool discovery via SSE transport
- Tool execution via SSE transport
- Server-Sent Events communication

### 9. Transport Comparison Tests
- Cross-transport consistency validation
- stdio vs HTTP transport comparison

## Running Tests

### Quick Start (Recommended)

Use the convenient test runner scripts to automatically set up the environment:

**Using Bash Script:**
```bash
cd /root/workspace/mimiclaw/test/integration
./run_tests.sh [options] [pytest_args]

# Examples:
./run_tests.sh -v                          # Run all tests with verbose output
./run_tests.sh test_mcp_integration.py::TestEndToEndScenarios -v  # Run specific test class
./run_tests.sh test_mcp_integration.py::TestSubagentTool -v       # Run subagent tool tests
./run_tests.sh test_mcp_integration.py::TestSubagentWorkflows -v  # Run subagent workflow tests
./run_tests.sh --no-venv -x -s             # Use system Python, stop on first failure
```

**Using Python Script:**
```bash
cd /root/workspace/mimiclaw/test/integration
python3 run_tests.py [options] [pytest_args]

# Example with virtual environment auto-creation:
python3 run_tests.py -v
```

Both scripts support these options:
- `--no-venv`: Skip virtual environment creation (use system Python)
- `--clean`: Clean up virtual environment before running
- `--help`: Show help message

### Manual Setup

If you prefer to set up the environment manually:

1. **Build the mimiclaw binary** (required):
   ```bash
   cd /root/workspace/mimiclaw
   # Follow build instructions for your platform
   ```

2. **Install Python dependencies**:
   ```bash
   cd /root/workspace/mimiclaw/test/integration
   pip install -r requirements.txt
   
   # Or in this specific environment:
   pip install --break-system-packages -r requirements.txt --target=/tmp/pypkgs
   export PYTHONPATH="/tmp/pypkgs:/root/workspace/mimiclaw/test/integration"
   ```

3. **Run pytest directly**:
   ```bash
   # Run all tests
   python3 -m pytest test_mcp_integration.py -v

   # Run only discovery tests
   python3 -m pytest test_mcp_integration.py::TestMCPDiscovery -v

   # Run only end-to-end tests
   python3 -m pytest test_mcp_integration.py::TestEndToEndScenarios -v

   # Run with detailed output
   python3 -m pytest test_mcp_integration.py -v -s
   ```

## Using the Virtual LLM Server

The virtual LLM server provides a REST API for dynamic response configuration:

### API Endpoints

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/v1/chat/completions` | POST | OpenAI-compatible chat completion endpoint |
| `/v1/models` | GET | List available models |
| `/test/reset` | POST | Reset all server state |
| `/test/response/queue` | POST | Add custom response to queue |
| `/test/tool_call` | POST | Configure server to respond with tool call |
| `/test/stats` | GET | Get server statistics |
| `/test/requests` | GET | Get captured request history |

### Example: Setting Tool Call Response

```python
import requests

# Configure LLM to respond with a tool call
response = requests.post(
    "http://localhost:9999/test/tool_call",
    json={
        "tool_name": "mcp::test_server::echo",
        "arguments": {"message": "Hello World"},
        "count": 1
    }
)
```

## Test Fixtures (conftest.py)

### Session-Scoped Fixtures

- `llm_server` - Starts a single LLM server for the entire test session with dynamic port allocation

### Function-Scoped Fixtures

- `llm_server_clean` - Resets LLM server state before each test
- `test_config` - Generates fresh test configuration with correct LLM port
- `mimiclaw_runner` - Wrapper for executing mimiclaw binary with test config

### HTTP/SSE Transport Fixtures

- `mcp_http_server` - Starts MCP server with HTTP transport (streamable-http mode)
- `mcp_sse_server` - Starts MCP server with SSE transport
- `test_config_http` - Configuration for HTTP MCP server testing
- `test_config_sse` - Configuration for SSE MCP server testing
- `mimiclaw_runner_http` - Runner for HTTP MCP server tests
- `mimiclaw_runner_sse` - Runner for SSE MCP server tests

## Writing New Tests

### Basic Test Structure

```python
def test_example(self, llm_server_clean, test_config, mimiclaw_runner):
    # 1. Reset state (automatically handled by fixture)
    # 2. Configure LLM responses
    llm_server_clean.set_text_response("Hello from LLM!")
    
    # 3. Run mimiclaw with user input
    result = mimiclaw_runner.run_with_input("Hello, world!")
    
    # 4. Assertions
    assert result["returncode"] == 0
    stats = llm_server_clean.get_stats()
    assert stats["call_count"] >= 1
```

### Testing Tool Calls

```python
def test_tool_call(self, llm_server_clean, test_config, mimiclaw_runner):
    # Configure LLM to make a tool call
    llm_server_clean.set_tool_call(
        "mcp::test_server::echo",
        {"message": "Test message"}
    )
    
    # Configure final text response after tool execution
    llm_server_clean.set_text_response("Tool executed successfully!")
    
    result = mimiclaw_runner.run_with_input("Please echo a message")
    
    # Verify LLM was called
    stats = llm_server_clean.get_stats()
    assert stats["call_count"] >= 1
```

### Testing HTTP Transport

```python
def test_http_transport(self, llm_server_clean, test_config_http, mimiclaw_runner_http):
    # Configure LLM to make a tool call via HTTP transport
    llm_server_clean.set_tool_call(
        "mcp::test_server_http::echo",
        {"message": "HTTP Transport Test"}
    )
    
    llm_server_clean.set_text_response("HTTP transport tool call completed!")
    
    result = mimiclaw_runner_http.run_with_input(
        "Use the echo tool via HTTP transport"
    )
    
    assert result["returncode"] in [0, 1]
```

### Testing SSE Transport

```python
def test_sse_transport(self, llm_server_clean, test_config_sse, mimiclaw_runner_sse):
    # Configure LLM to make a tool call via SSE transport
    llm_server_clean.set_tool_call(
        "mcp::test_server_sse::echo",
        {"message": "SSE Transport Test"}
    )
    
    llm_server_clean.set_text_response("SSE transport tool call completed!")
    
    result = mimiclaw_runner_sse.run_with_input(
        "Use the echo tool via SSE transport"
    )
    
    assert result["returncode"] in [0, 1]
```

## Troubleshooting

### Common Issues

1. **Port Conflicts** - The framework uses dynamic port allocation, but check for leftover processes:
   ```bash
   pkill -f "virtual_llm_server\|mimiclaw\|test_mcp"
   ```

2. **MCP Server Not Starting** - Check Python path and permissions:
   ```bash
   which python3
   ls -la /root/workspace/mimiclaw/test/integration/test_mcp_server.py
   ```

3. **LLM Call Count is 0** - Ensure input ends with newline (fixture handles this automatically)

### Debug Mode

To see detailed output during test execution:

```bash
python3 -m pytest test_mcp_integration.py -v -s --timeout=60
```

## Manual Testing with Test Controller

For manual testing or debugging, use the test controller to start a persistent test environment:

```bash
# Start test environment (LLM server + config)
cd /root/workspace/mimiclaw/test/integration
python3 test_controller.py --action start

# In another terminal, run mimiclaw manually
/root/workspace/mimiclaw/build/mimiclaw /root/workspace/mimiclaw/test/integration/config/test_config.json

# Stop the test environment (Ctrl+C or)
python3 test_controller.py --action stop
```

## Design Principles

1. **Black-box Testing** - Tests interact with the actual mimiclaw binary, no mocking of internal components
2. **Test Isolation** - Each test starts with clean server state and fresh configuration
3. **Dynamic Responses** - LLM server responses are configured per test case for flexible scenario testing
4. **Realistic Environment** - Uses actual MCP server stdio communication and OpenAI API protocol
5. **Standard Protocol** - Test MCP server uses official MCP Python SDK to ensure protocol compliance
6. **Multi-Transport Support** - Tests cover stdio, HTTP, and SSE transport modes for comprehensive MCP compatibility

## MCP Server Transport Modes

The test MCP server supports multiple transport modes for testing different MCP connection scenarios:

### Transport Types

| Transport | Description | Use Case |
|-----------|-------------|----------|
| `stdio` | Standard input/output communication (default) | Local process-based MCP servers |
| `sse` | Server-Sent Events over HTTP | Real-time notifications, web-based clients |
| `streamable-http` | HTTP with streaming support | RESTful MCP servers with streaming |

### Running MCP Server with Different Transports

```bash
# stdio mode (default)
python3 test_mcp_server.py --transport stdio

# SSE mode
python3 test_mcp_server.py --transport sse --port 8000

# HTTP streaming mode
python3 test_mcp_server.py --transport streamable-http --port 8000
```

### Available Test Tools

The test MCP server provides these tools for testing:

| Tool | Description | Parameters |
|------|-------------|------------|
| `echo` | Echo back a message | `message: str` |
| `add` | Add two numbers | `a: float, b: float` |
| `get_test_data` | Return structured test data | None |
| `slow_operation` | Simulate slow operation | `delay_ms: int` |
| `error_test` | Return error response | `error_type: str` |
| `validate_params` | Validate parameter passing | `required_param: str, optional_param: int` |
| `transport_info` | Return current transport mode info | None |
