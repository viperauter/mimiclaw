#!/usr/bin/env python3
"""
Test MCP Server - Model Context Protocol implementation for testing
Uses official MCP Python SDK (https://github.com/modelcontextprotocol/python-sdk)
Supports multiple transport modes: stdio, sse, streamable-http
"""

import asyncio
import json
import time
import argparse
import os
from typing import Any

from mcp.server.fastmcp import FastMCP


def create_server(port: int = 8000):
    """Create and configure the MCP server with tools"""
    mcp = FastMCP("Test MCP Server", port=port)

    @mcp.tool()
    def echo(message: str) -> str:
        """Echo back the input message - used for basic connectivity testing
        
        Args:
            message: The message to echo back
            
        Returns:
            The echoed message with prefix
        """
        return f"Echo: {message}"

    @mcp.tool()
    def add(a: float, b: float) -> str:
        """Add two numbers together and return the result
        
        Args:
            a: First number to add
            b: Second number to add
            
        Returns:
            The addition result as a formatted string
        """
        return f"Result: {a} + {b} = {a + b}"

    @mcp.tool()
    def get_test_data() -> dict[str, Any]:
        """Return structured test data for validation
        
        Returns:
            A dictionary containing test data with status, data array, and timestamp
        """
        return {
            "status": "success",
            "data": [1, 2, 3, 4, 5],
            "timestamp": "2024-01-01T00:00:00Z"
        }

    @mcp.tool()
    async def slow_operation(delay_ms: int = 1000) -> str:
        """Simulate a slow operation for timeout testing
        
        Args:
            delay_ms: Delay in milliseconds before responding (default: 1000)
            
        Returns:
            A message indicating the operation completed after the delay
        """
        await asyncio.sleep(delay_ms / 1000.0)
        return f"Completed after {delay_ms}ms delay"

    @mcp.tool()
    def error_test(error_type: str = "generic") -> dict[str, Any]:
        """Return an error response for testing error handling
        
        Args:
            error_type: Type of error to simulate (generic, not_found, permission)
            
        Returns:
            An error response dictionary
        """
        error_messages = {
            "generic": "An error occurred",
            "not_found": "Resource not found",
            "permission": "Permission denied"
        }
        return {
            "status": "error",
            "error": error_messages.get(error_type, "Unknown error"),
            "error_type": error_type
        }

    @mcp.tool()
    def validate_params(required_param: str, optional_param: int = 42) -> dict[str, Any]:
        """Validate parameter passing and type conversion
        
        Args:
            required_param: A required string parameter
            optional_param: An optional integer parameter (default: 42)
            
        Returns:
            A dictionary showing the received parameters
        """
        return {
            "status": "validated",
            "required_param": required_param,
            "optional_param": optional_param,
            "required_type": type(required_param).__name__,
            "optional_type": type(optional_param).__name__
        }

    @mcp.tool()
    def transport_info() -> dict[str, Any]:
        """Return information about the current transport mode
        
        Returns:
            A dictionary containing transport information
        """
        return {
            "status": "ok",
            "transport": os.environ.get("MCP_TRANSPORT", "unknown"),
            "server_name": "Test MCP Server",
            "tools_count": 7
        }

    return mcp


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Test MCP Server using official MCP Python SDK")
    parser.add_argument(
        "--transport", 
        choices=["stdio", "sse", "streamable-http"], 
        default="stdio",
        help="Transport mode: stdio (default), sse, or streamable-http"
    )
    parser.add_argument(
        "--port",
        type=int,
        default=8000,
        help="Port for HTTP/SSE transport (default: 8000)"
    )
    parser.add_argument(
        "--host",
        default="localhost",
        help="Host for HTTP/SSE transport (default: localhost)"
    )
    args = parser.parse_args()
    
    os.environ["MCP_TRANSPORT"] = args.transport
    
    mcp = create_server(port=args.port)
    
    if args.transport == "stdio":
        mcp.run(transport="stdio")
    elif args.transport == "sse":
        mcp.run(transport="sse")
    elif args.transport == "streamable-http":
        mcp.run(transport="streamable-http")
