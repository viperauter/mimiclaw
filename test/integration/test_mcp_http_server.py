#!/usr/bin/env python3
"""
Test MCP HTTP Server - Model Context Protocol implementation for testing
Supports Streamable HTTP transport mode for MCP protocol

Based on MCP Streamable HTTP Transport specification:
https://modelcontextprotocol.io/specification/2025-06-18/basic/transports
"""

import sys
import json
import argparse
import uuid
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse
import threading


class MCPHTTPServer:
    """MCP (Model Context Protocol) test server with HTTP transport"""

    def __init__(self):
        self._tools = self._define_tools()
        self._sessions = {}
        self._protocol_version = "2025-06-18"

    def _define_tools(self):
        """Define available tools"""
        return [
            {
                "name": "echo",
                "description": "Echo back the input message - used for basic connectivity testing",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "message": {
                            "type": "string",
                            "description": "The message to echo back"
                        }
                    },
                    "required": ["message"]
                }
            },
            {
                "name": "add",
                "description": "Add two numbers together and return the result",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "a": {"type": "number", "description": "First number to add"},
                        "b": {"type": "number", "description": "Second number to add"}
                    },
                    "required": ["a", "b"]
                }
            },
            {
                "name": "get_test_data",
                "description": "Return structured test data for validation",
                "inputSchema": {
                    "type": "object",
                    "properties": {},
                    "required": []
                }
            },
            {
                "name": "slow_operation",
                "description": "Simulate a slow operation for timeout testing",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "delay_ms": {
                            "type": "number",
                            "description": "Delay in milliseconds before responding"
                        }
                    },
                    "required": []
                }
            }
        ]

    def _send_jsonrpc_response(self, handler, msg_id, result=None, error=None):
        """Send JSON-RPC response"""
        response = {
            "jsonrpc": "2.0",
            "id": msg_id
        }
        if result is not None:
            response["result"] = result
        if error is not None:
            response["error"] = error

        response_body = json.dumps(response, ensure_ascii=False).encode("utf-8")
        handler.send_response(200 if error is None else 400)
        handler.send_header("Content-Type", "application/json")
        handler.send_header("Content-Length", len(response_body))
        handler.end_headers()
        handler.wfile.write(response_body)

    def _send_sse_event(self, handler, event_id, event_type, data):
        """Send Server-Sent Event"""
        event = f"id: {event_id}\n"
        event += f"event: {event_type}\n"
        event += f"data: {json.dumps(data, ensure_ascii=False)}\n\n"
        return event.encode("utf-8")

    def _handle_initialize(self, session_id, msg_id, params):
        """Handle MCP initialize request"""
        print(f"[MCP HTTP Server] Initialize request: {params}", file=sys.stderr)
        session_id_new = str(uuid.uuid4())
        result = {
            "protocolVersion": self._protocol_version,
            "capabilities": {
                "tools": {}
            },
            "serverInfo": {
                "name": "Test MCP HTTP Server",
                "version": "1.0.0"
            }
        }
        return session_id_new, result

    def _handle_list_tools(self, session_id, msg_id, params):
        """Handle tools/list request"""
        print(f"[MCP HTTP Server] List tools request", file=sys.stderr)
        result = {
            "tools": self._tools
        }
        return None, result

    def _handle_call_tool(self, session_id, msg_id, params):
        """Handle tools/call request"""
        tool_name = params.get("name")
        arguments = params.get("arguments", {})

        print(f"[MCP HTTP Server] Calling tool: {tool_name} with args: {arguments}", file=sys.stderr)

        result = None
        if tool_name == "echo":
            message = arguments.get("message", "")
            result = {
                "content": [
                    {
                        "type": "text",
                        "text": f"Echo: {message}"
                    }
                ],
                "isError": False
            }

        elif tool_name == "add":
            a = arguments.get("a", 0)
            b = arguments.get("b", 0)
            result = {
                "content": [
                    {
                        "type": "text",
                        "text": f"Result: {a} + {b} = {a + b}"
                    }
                ],
                "isError": False
            }

        elif tool_name == "get_test_data":
            result = {
                "content": [
                    {
                        "type": "text",
                        "text": json.dumps({
                            "status": "success",
                            "data": [1, 2, 3, 4, 5],
                            "timestamp": "2024-01-01T00:00:00Z"
                        })
                    }
                ],
                "isError": False
            }

        elif tool_name == "slow_operation":
            import time
            delay_ms = arguments.get("delay_ms", 1000)
            time.sleep(delay_ms / 1000.0)
            result = {
                "content": [
                    {
                        "type": "text",
                        "text": f"Completed after {delay_ms}ms delay"
                    }
                ],
                "isError": False
            }

        else:
            return None, None, {
                "code": -32602,
                "message": f"Unknown tool: {tool_name}"
            }

        return None, result, None

    def _handle_ping(self, session_id, msg_id, params):
        """Handle ping request"""
        return None, {}, None

    def process_message(self, msg):
        """Process a single incoming JSON-RPC message"""
        msg_id = msg.get("id")
        method = msg.get("method")
        params = msg.get("params", {})
        session_id = msg.get("session_id")

        error = None
        result = None
        new_session_id = None

        if method == "initialize":
            new_session_id, result = self._handle_initialize(session_id, msg_id, params)
        elif method == "tools/list":
            new_session_id, result, error = self._handle_list_tools(session_id, msg_id, params)
        elif method == "tools/call":
            new_session_id, result, error = self._handle_call_tool(session_id, msg_id, params)
        elif method == "ping":
            new_session_id, result, error = self._handle_ping(session_id, msg_id, params)
        elif method == "notifications/initialized":
            return None, None, None
        else:
            error = {
                "code": -32601,
                "message": f"Method not found: {method}"
            }

        return new_session_id, result, error


class MCPHTTPRequestHandler(BaseHTTPRequestHandler):
    """HTTP request handler for MCP Streamable HTTP transport"""

    server: MCPHTTPServer

    def log_message(self, format, *args):
        """Override to use stderr for logging"""
        print(f"[MCP HTTP Server] {format % args}", file=sys.stderr)

    def _get_session_id(self):
        """Get session ID from request headers"""
        return self.headers.get("Mcp-Session-Id")

    def _create_sse_stream(self, session_id, response_data):
        """Create SSE stream for response"""
        event_id = str(uuid.uuid4())
        return self.server._send_sse_event(self, event_id, "message", response_data)

    def do_POST(self):
        """Handle POST requests - send JSON-RPC messages"""
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length) if content_length > 0 else b""

        print(f"[MCP HTTP Server] POST received, content length: {content_length}", file=sys.stderr)

        if not body:
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            error_body = json.dumps({
                "jsonrpc": "2.0",
                "error": {"code": -32700, "message": "Parse error"}
            }).encode()
            self.send_header("Content-Length", len(error_body))
            self.end_headers()
            self.wfile.write(error_body)
            return

        try:
            msg = json.loads(body.decode("utf-8"))
        except json.JSONDecodeError as e:
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            error_body = json.dumps({
                "jsonrpc": "2.0",
                "error": {"code": -32700, "message": f"Parse error: {e}"}
            }).encode()
            self.send_header("Content-Length", len(error_body))
            self.end_headers()
            self.wfile.write(error_body)
            return

        msg_method = msg.get("method", "")
        is_notification = msg.get("id") is None

        if is_notification:
            self.server.process_message(msg)
            self.send_response(202)
            self.send_header("Content-Length", 0)
            self.end_headers()
            return

        new_session_id, result, error = self.server.process_message(msg)

        response_data = {"jsonrpc": "2.0", "id": msg.get("id")}
        if result is not None:
            response_data["result"] = result
        if error is not None:
            response_data["error"] = error

        if new_session_id:
            response_body = json.dumps(response_data, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", len(response_body))
            self.send_header("Mcp-Session-Id", new_session_id)
            self.end_headers()
            self.wfile.write(response_body)
        else:
            sse_event = self._create_sse_stream(self._get_session_id(), response_data)
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.send_header("Content-Length", len(sse_event))
            if new_session_id:
                self.send_header("Mcp-Session-Id", new_session_id)
            self.end_headers()
            self.wfile.write(sse_event)

    def do_GET(self):
        """Handle GET requests - open SSE stream for server-to-client messages"""
        print(f"[MCP HTTP Server] GET received (SSE stream)", file=sys.stderr)

        session_id = self._get_session_id()
        if not session_id:
            session_id = str(uuid.uuid4())
            print(f"[MCP HTTP Server] Created new session: {session_id}", file=sys.stderr)

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("Mcp-Session-Id", session_id)
        self.end_headers()

        endpoint_event = self.server._send_sse_event(
            self, session_id, "endpoint", {"path": "/mcp"}
        )
        self.wfile.write(endpoint_event)
        self.wfile.flush()

        try:
            while True:
                import time
                heartbeat = f": heartbeat\n\n".encode()
                self.wfile.write(heartbeat)
                self.wfile.flush()
                time.sleep(30)
        except (BrokenPipeError, ConnectionResetError):
            pass

        print(f"[MCP HTTP Server] SSE stream closed for session {session_id}", file=sys.stderr)

    def do_DELETE(self):
        """Handle DELETE requests - terminate session"""
        session_id = self._get_session_id()
        print(f"[MCP HTTP Server] DELETE session: {session_id}", file=sys.stderr)
        self.send_response(405)
        self.send_header("Content-Length", 0)
        self.end_headers()


def run_server(port):
    """Run MCP HTTP server"""
    server = MCPHTTPServer()
    handler = lambda *args, **kwargs: MCPHTTPRequestHandler(*args, server=server, **kwargs)
    httpd = HTTPServer(("localhost", port), handler)

    print(f"[MCP HTTP Server] Test MCP HTTP Server running on http://localhost:{port}/mcp", file=sys.stderr)
    print(f"[MCP HTTP Server] Available tools: {[t['name'] for t in server._tools]}", file=sys.stderr)
    sys.stderr.flush()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[MCP HTTP Server] Shutting down...", file=sys.stderr)
        httpd.shutdown()


def main():
    parser = argparse.ArgumentParser(description="Test MCP HTTP Server")
    parser.add_argument("--port", type=int, default=8080, help="Port to listen on")
    parser.add_argument("--mode", choices=["http"], default="http",
                        help="Transport mode (http is the only supported mode)")
    args = parser.parse_args()

    run_server(args.port)


if __name__ == "__main__":
    main()
