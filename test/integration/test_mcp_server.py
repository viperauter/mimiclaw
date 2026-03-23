#!/usr/bin/env python3
"""
Test MCP Server - Model Control Plane implementation for testing
Supports stdio transport mode for MCP protocol
"""

import sys
import json
import argparse

class MCPServer:
    """MCP (Model Control Plane) test server implementation"""
    
    def __init__(self):
        self._tools = self._define_tools()
        
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
    
    def _send_jsonrpc(self, msg_id, result=None, error=None):
        """Send JSON-RPC response via stdout"""
        response = {
            "jsonrpc": "2.0",
            "id": msg_id
        }
        if result is not None:
            response["result"] = result
        if error is not None:
            response["error"] = error
        
        json_str = json.dumps(response, ensure_ascii=False)
        print(json_str, flush=True)
        sys.stderr.write(f"[MCP Server] Sent response: {json_str[:200]}...\n")
        
    def _handle_initialize(self, msg_id, params):
        """Handle MCP initialize request"""
        sys.stderr.write(f"[MCP Server] Initialize request: {params}\n")
        result = {
            "protocolVersion": "2024-11-05",
            "capabilities": {
                "tools": {}
            },
            "serverInfo": {
                "name": "Test MCP Server",
                "version": "1.0.0"
            }
        }
        self._send_jsonrpc(msg_id, result)
        
    def _handle_list_tools(self, msg_id, params):
        """Handle tools/list request"""
        result = {
            "tools": self._tools
        }
        self._send_jsonrpc(msg_id, result)
        
    def _handle_call_tool(self, msg_id, params):
        """Handle tools/call request"""
        tool_name = params.get("name")
        arguments = params.get("arguments", {})
        
        sys.stderr.write(f"[MCP Server] Calling tool: {tool_name} with args: {arguments}\n")
        
        result = None
        if tool_name == "echo":
            message = arguments.get("message", "")
            result = {
                "content": [
                    {
                        "type": "text",
                        "text": f"Echo: {message}"
                    }
                ]
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
                ]
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
                ]
            }
            
        elif tool_name == "slow_operation":
            delay_ms = arguments.get("delay_ms", 1000)
            import time
            time.sleep(delay_ms / 1000.0)
            result = {
                "content": [
                    {
                        "type": "text",
                        "text": f"Completed after {delay_ms}ms delay"
                    }
                ]
            }
            
        else:
            self._send_jsonrpc(msg_id, None, {
                "code": -32602,
                "message": f"Unknown tool: {tool_name}"
            })
            return
            
        self._send_jsonrpc(msg_id, result)
        
    def _handle_ping(self, msg_id, params):
        """Handle ping request"""
        self._send_jsonrpc(msg_id, {})
        
    def process_message(self, line):
        """Process a single incoming message"""
        if not line or not line.strip():
            return
            
        try:
            msg = json.loads(line.strip())
        except json.JSONDecodeError as e:
            sys.stderr.write(f"[MCP Server] Invalid JSON: {line[:100]}... Error: {e}\n")
            return
            
        msg_id = msg.get("id")
        method = msg.get("method")
        
        if method == "initialize":
            self._handle_initialize(msg_id, msg.get("params", {}))
        elif method == "tools/list":
            self._handle_list_tools(msg_id, msg.get("params", {}))
        elif method == "tools/call":
            self._handle_call_tool(msg_id, msg.get("params", {}))
        elif method == "ping":
            self._handle_ping(msg_id, msg.get("params", {}))
        elif method == "notifications/initialized":
            pass  # Notification - no response needed
        else:
            sys.stderr.write(f"[MCP Server] Unknown method: {method}\n")
            if msg_id:
                self._send_jsonrpc(msg_id, None, {
                    "code": -32601,
                    "message": f"Method not found: {method}"
                })
                
    def run_stdio(self):
        """Run server in stdio mode"""
        sys.stderr.write("[MCP Server] Test MCP Server running in stdio mode\n")
        sys.stderr.write(f"[MCP Server] Available tools: {[t['name'] for t in self._tools]}\n")
        sys.stderr.flush()
        
        for line in sys.stdin:
            self.process_message(line)

def main():
    parser = argparse.ArgumentParser(description="Test MCP Server")
    parser.add_argument("--mode", choices=["stdio"], default="stdio", 
                       help="Transport mode (stdio is the only supported mode)")
    args = parser.parse_args()
    
    server = MCPServer()
    
    if args.mode == "stdio":
        server.run_stdio()

if __name__ == "__main__":
    main()
