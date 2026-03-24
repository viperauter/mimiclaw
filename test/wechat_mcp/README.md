# WeChat MCP Integration Tests

This directory contains integration tests for verifying mimiclaw can connect to and use the WeChat MCP server.

## Prerequisites

1. **bun** must be installed (for running `bunx mcp-wechat-server`)
2. **mimiclaw** binary must be built
3. Python dependencies from parent `test/integration` directory

## Test Structure

- `conftest.py` - pytest fixtures and configuration
- `test_wechat_mcp.py` - Main test suite
- `virtual_llm_server.py` - Mock LLM server for testing

## WeChat MCP Configuration

The tests use this MCP configuration:

```json
{
  "mcpServers": {
    "wechat": {
      "command": "bunx",
      "args": ["mcp-wechat-server"]
    }
  }
}
```

## Running Tests

```bash
cd /home/aotao/workspace/mimiclaw/test/wechat_mcp

# Copy virtual LLM server if not present
cp ../integration/virtual_llm_server.py .

# Install dependencies
pip install flask requests pytest

# Run tests
python -m pytest test_wechat_mcp.py -v -s
```

## Test Categories

1. **Discovery Tests** - Verify WeChat MCP server is properly configured
2. **Execution Tests** - Test calling WeChat MCP tools (get_qrcode_status, send_text_message)
3. **Message Handling Tests** - Test receiving messages via wechat_get_messages
4. **End-to-End Tests** - Complete workflow tests

## WeChat MCP Tools

The WeChat MCP server provides these tools:

- `wechat_get_qrcode_status` - Check QR code login status
- `wechat_send_text_message` - Send text message to a user
- `wechat_get_messages` - Get received messages
- (and more depending on server implementation)

## Notes

- The WeChat bot must be logged in before running tests
- Tests use a virtual LLM server to control AI responses
- MCP tools are called with prefix `mcp::wechat::`