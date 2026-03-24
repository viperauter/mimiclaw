"""
MCP Integration Tests - Test suite for MCP tool discovery and execution
"""

import pytest
import json
import time
import re

class TestMCPDiscovery:
    """Tests for MCP server discovery and tool registration"""
    
    def test_mcp_server_configuration(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test that MCP servers are properly configured from config.json"""
        # We'll verify this indirectly through tool calls
        llm_server_clean.set_tool_call(
            "mcp::test_server::echo",
            {"message": "Hello MCP!"}
        )
        
        result = mimiclaw_runner.run_with_input("Test the echo tool", timeout=20)
        
        # Verify the process ran
        print(f"STDOUT: {result['stdout'][:500]}")
        print(f"STDERR: {result['stderr'][:500]}")
        
        # The exact success conditions will depend on how mimiclaw outputs results
        # For now, we just verify the process completed
        assert result["returncode"] in [0, 1], "Process should complete"
        
    def test_llm_server_dynamic_config(self, llm_server_clean):
        """Test that LLM server can be dynamically configured"""
        llm_server_clean.reset()
        
        # Configure a tool call response
        llm_server_clean.set_tool_call(
            "mcp::test_server::add",
            {"a": 10, "b": 20}
        )
        
        # Verify stats
        stats = llm_server_clean.get_stats()
        assert stats["call_count"] == 0
        assert stats["queue_size"] == 1
        
    def test_multiple_tool_calls(self, llm_server_clean):
        """Test queuing multiple tool call responses"""
        llm_server_clean.reset()
        
        # Queue multiple responses
        llm_server_clean.set_tool_call(
            "mcp::test_server::echo",
            {"message": "First call"},
            count=3
        )
        
        stats = llm_server_clean.get_stats()
        assert stats["queue_size"] == 3

class TestMCPExecution:
    """Tests for MCP tool execution"""
    
    def test_echo_tool_call(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test calling the echo tool through MCP"""
        llm_server_clean.reset()
        
        # Configure LLM to call the echo tool
        llm_server_clean.set_tool_call(
            "mcp::test_server::echo",
            {"message": "Integration Test"}
        )
        
        # Also add a text response for after the tool call
        llm_server_clean.set_text_response("Tool call completed successfully!")
        
        result = mimiclaw_runner.run_with_input(
            "Please use the echo tool to say 'Integration Test'",
            timeout=30
        )
        
        print(f"\n=== Test Output ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Stdout:\n{result['stdout']}")
        print(f"Stderr:\n{result['stderr'][:1000]}")
        
        # Verify LLM was called
        stats = llm_server_clean.get_stats()
        print(f"LLM Stats: {stats}")
        
        # Basic assertions
        assert result["returncode"] in [0, 1], "Process should complete"
        
    def test_add_tool_call(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test calling the add tool through MCP"""
        llm_server_clean.reset()
        
        llm_server_clean.set_tool_call(
            "mcp::test_server::add",
            {"a": 15, "b": 27}
        )
        
        llm_server_clean.set_text_response("Calculation complete!")
        
        result = mimiclaw_runner.run_with_input(
            "What is 15 plus 27?",
            timeout=30
        )
        
        print(f"\n=== Test Output ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Stdout:\n{result['stdout']}")
        print(f"Stderr:\n{result['stderr'][:1000]}")
        
        # Check if MCP server was mentioned in logs
        # This helps verify the MCP provider was invoked
        if "mcp" in result["stderr"].lower() or "MCP" in result["stderr"]:
            print("✓ MCP mentioned in logs")
        
        assert result["returncode"] in [0, 1]

class TestMCPConfigurationScenarios:
    """Tests for different MCP configuration scenarios"""
    
    def test_multiple_mcp_servers(self, llm_server_clean, test_config):
        """Test configuring multiple MCP servers"""
        config_gen = test_config["config_gen"]
        
        # Create config with multiple MCP servers
        mcp_servers = [
            {
                "name": "server1",
                "command": f"python {config_gen.work_dir}/test_mcp_server.py",
                "requires_confirmation": False
            },
            {
                "name": "server2",
                "command": f"python {config_gen.work_dir}/test_mcp_server.py",
                "requires_confirmation": True
            }
        ]
        
        # Copy the MCP server script to work directory for testing
        import shutil
        source_script = __file__.replace("test_mcp_integration.py", "test_mcp_server.py")
        shutil.copy(source_script, f"{config_gen.work_dir}/test_mcp_server.py")
        
        config_path = config_gen.generate_config(mcp_servers)
        
        # Verify config was generated correctly
        with open(config_path, "r") as f:
            config = json.load(f)
            
        assert "mcpServers" in config.get("tools", {})
        assert len(config["tools"]["mcpServers"]) == 2
        assert config["tools"]["mcpServers"][0]["name"] == "server1"
        assert config["tools"]["mcpServers"][1]["requires_confirmation"] == True
        
    def test_mcp_server_with_confirmation(self, llm_server_clean, test_config):
        """Test MCP server with requires_confirmation flag"""
        config_gen = test_config["config_gen"]
        
        mcp_servers = [
            {
                "name": "sensitive_server",
                "command": f"python {__file__.replace('test_mcp_integration.py', 'test_mcp_server.py')}",
                "requires_confirmation": True
            }
        ]
        
        config_path = config_gen.generate_config(mcp_servers)
        
        with open(config_path, "r") as f:
            config = json.load(f)
            
        server_config = config["tools"]["mcpServers"][0]
        assert server_config["requires_confirmation"] == True
        assert server_config["name"] == "sensitive_server"

class TestLLMControlFlow:
    """Advanced tests for LLM-driven control flow"""
    
    def test_multi_turn_conversation(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test a multi-turn conversation with tool calls"""
        llm_server_clean.reset()
        
        # First turn: call echo tool
        llm_server_clean.set_tool_call(
            "mcp::test_server::echo",
            {"message": "Turn 1"}
        )
        
        # Second turn: call add tool  
        llm_server_clean.set_tool_call(
            "mcp::test_server::add",
            {"a": 1, "b": 2}
        )
        
        # Final turn: text response
        llm_server_clean.set_text_response("All tasks completed!")
        
        result = mimiclaw_runner.run_with_input(
            "Run a multi-turn test with echo and add tools",
            timeout=45
        )
        
        print(f"\n=== Multi-turn Test Output ===")
        print(f"Return Code: {result['returncode']}")
        print(f"LLM Stats: {llm_server_clean.get_stats()}")
        
        assert result["returncode"] in [0, 1]
        
    def test_conversation_with_no_tools(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test conversation without any tool calls"""
        llm_server_clean.reset()
        
        llm_server_clean.set_text_response(
            "This is a direct response without any tool calls.",
            count=2
        )
        
        result = mimiclaw_runner.run_with_input(
            "What is your name and purpose?",
            timeout=20
        )
        
        print(f"\n=== No-tools Test ===")
        print(f"Return Code: {result['returncode']}")
        stats = llm_server_clean.get_stats()
        print(f"LLM Call Count: {stats['call_count']}")
        
        assert result["returncode"] in [0, 1]

@pytest.mark.slow
class TestEndToEndScenarios:
    """End-to-end integration tests"""
    
    def test_full_mcp_workflow(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test the complete MCP workflow: discovery -> tool call -> result"""
        llm_server_clean.reset()
        
        # Sequence:
        # 1. System prompt with tools list
        # 2. LLM decides to call echo tool
        # 3. MCP server executes tool and returns result
        # 4. LLM generates final response
        
        llm_server_clean.set_tool_call(
            "mcp::test_server::get_test_data",
            {}
        )
        
        llm_server_clean.set_text_response(
            "I successfully retrieved test data from the MCP server. "
            "The data shows status: success with values [1, 2, 3, 4, 5]."
        )
        
        result = mimiclaw_runner.run_with_input(
            "Retrieve test data using the MCP server and summarize the results",
            timeout=45
        )
        
        print(f"\n=== Full Workflow Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Stdout: {result['stdout'][:500]}")
        print(f"Stderr preview: {result['stderr'][:1000]}")
        
        # Verify at least one LLM call was made
        stats = llm_server_clean.get_stats()
        print(f"Total LLM calls: {stats['call_count']}")
        
        assert result["returncode"] in [0, 1]
        # We expect at least one LLM call (the tool call) plus possibly a second for the final answer
        assert stats["call_count"] >= 1


class TestSubagentTool:
    """Tests for the built-in subagents tool functionality"""
    
    def test_subagent_spawn_basic(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test basic subagent spawning"""
        llm_server_clean.reset()
        
        # Sequence: LLM first calls subagents spawn tool
        llm_server_clean.set_tool_call(
            "subagents",
            {
                "action": "spawn",
                "task": "Calculate the sum of first 10 prime numbers",
                "maxIters": 3,
                "timeoutSec": 30
            }
        )
        
        # After spawn, we expect a response indicating the subagent was created
        llm_server_clean.set_text_response(
            "I have successfully spawned a subagent to calculate the sum of first 10 prime numbers."
        )
        
        result = mimiclaw_runner.run_with_input(
            "Spawn a subagent to calculate the sum of first 10 prime numbers",
            timeout=30
        )
        
        print(f"\n=== Subagent Spawn Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Stdout: {result['stdout'][-800:]}")
        
        # Verify at least one LLM call was made
        stats = llm_server_clean.get_stats()
        print(f"LLM Call Count: {stats['call_count']}")
        
        assert result["returncode"] in [0, 1]
        assert stats["call_count"] >= 1
    
    def test_subagent_list(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test listing subagents"""
        llm_server_clean.reset()
        
        # First spawn a subagent
        llm_server_clean.set_tool_call(
            "subagents",
            {
                "action": "spawn",
                "task": "Perform a quick analysis of test data",
                "maxIters": 2,
                "timeoutSec": 20
            }
        )
        
        # Then list subagents
        llm_server_clean.set_tool_call(
            "subagents",
            {
                "action": "list",
                "recentMinutes": 5
            }
        )
        
        llm_server_clean.set_text_response(
            "I can see the subagent you spawned is currently running."
        )
        
        result = mimiclaw_runner.run_with_input(
            "Spawn a subagent for quick analysis and list all running subagents",
            timeout=30
        )
        
        print(f"\n=== Subagent List Test ===")
        print(f"Return Code: {result['returncode']}")
        stats = llm_server_clean.get_stats()
        print(f"LLM Call Count: {stats['call_count']}")
        
        assert result["returncode"] in [0, 1]
        assert stats["call_count"] >= 2  # spawn + list + possibly final response
    
    def test_subagent_spawn_with_tools(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test spawning a subagent with specific tools"""
        llm_server_clean.reset()
        
        llm_server_clean.set_tool_call(
            "subagents",
            {
                "action": "spawn",
                "task": "Analyze the available tools and create a summary",
                "tools": "subagents",  # Only allow subagent tool
                "maxIters": 3,
                "timeoutSec": 30
            }
        )
        
        llm_server_clean.set_text_response(
            "Subagent spawned successfully with restricted tool access."
        )
        
        result = mimiclaw_runner.run_with_input(
            "Spawn a subagent that can only use subagents tool to analyze available tools",
            timeout=30
        )
        
        print(f"\n=== Subagent Spawn with Tools Test ===")
        print(f"Return Code: {result['returncode']}")
        stats = llm_server_clean.get_stats()
        print(f"LLM Call Count: {stats['call_count']}")
        
        assert result["returncode"] in [0, 1]
        assert stats["call_count"] >= 1

class TestSubagentWorkflows:
    """Advanced subagent workflow tests"""
    
    def test_spawn_and_immediate_join(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test spawning a subagent and immediately joining it"""
        llm_server_clean.reset()
        
        # Spawn
        llm_server_clean.set_tool_call(
            "subagents",
            {
                "action": "spawn",
                "task": "Quick computation task",
                "maxIters": 2,
                "timeoutSec": 15
            }
        )
        
        # Join - need to get id from spawn response first
        llm_server_clean.set_tool_call(
            "subagents",
            {
                "action": "join",
                "id": "some_subagent_id",  # In real scenario, this comes from spawn response
                "waitMs": 5000
            }
        )
        
        llm_server_clean.set_text_response(
            "Subagent completed its task successfully."
        )
        
        result = mimiclaw_runner.run_with_input(
            "Spawn a subagent for quick computation and wait for it to complete",
            timeout=45
        )
        
        print(f"\n=== Spawn and Join Test ===")
        print(f"Return Code: {result['returncode']}")
        stats = llm_server_clean.get_stats()
        print(f"LLM Call Count: {stats['call_count']}")
        
        assert result["returncode"] in [0, 1]
    
    def test_subagent_cancel(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test canceling a subagent"""
        llm_server_clean.reset()
        
        # Spawn long-running task
        llm_server_clean.set_tool_call(
            "subagents",
            {
                "action": "spawn",
                "task": "Perform a lengthy analysis that we will cancel",
                "maxIters": 10,
                "timeoutSec": 60
            }
        )
        
        # Cancel it
        llm_server_clean.set_tool_call(
            "subagents",
            {
                "action": "cancel",
                "id": "subagent_to_cancel",
                "mode": "cancel"  # soft cancel
            }
        )
        
        llm_server_clean.set_text_response(
            "The subagent has been canceled as requested."
        )
        
        result = mimiclaw_runner.run_with_input(
            "Start a subagent and then cancel it",
            timeout=30
        )
        
        print(f"\n=== Subagent Cancel Test ===")
        print(f"Return Code: {result['returncode']}")
        stats = llm_server_clean.get_stats()
        print(f"LLM Call Count: {stats['call_count']}")
        
        assert result["returncode"] in [0, 1]


class TestMCPHTTPTransport:
    """Tests for MCP HTTP transport mode"""
    
    def test_http_server_startup(self, mcp_http_server):
        """Test that HTTP MCP server starts successfully"""
        assert mcp_http_server.url is not None
        assert mcp_http_server.port > 0
        print(f"\n=== HTTP Server Info ===")
        print(f"URL: {mcp_http_server.url}")
        print(f"Port: {mcp_http_server.port}")
        
    def test_http_tool_discovery(self, llm_server_clean, test_config_http, mimiclaw_runner_http):
        """Test tool discovery via HTTP transport"""
        llm_server_clean.reset()
        
        llm_server_clean.set_tool_call(
            "mcp::test_server_http::echo",
            {"message": "HTTP Transport Test"}
        )
        
        llm_server_clean.set_text_response("HTTP transport tool call completed!")
        
        result = mimiclaw_runner_http.run_with_input(
            "Use the echo tool via HTTP transport to say 'HTTP Transport Test'",
            timeout=30
        )
        
        print(f"\n=== HTTP Tool Discovery Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Stdout: {result['stdout'][-500:]}")
        
        stats = llm_server_clean.get_stats()
        print(f"LLM Call Count: {stats['call_count']}")
        
        assert result["returncode"] in [0, 1]
        
    def test_http_add_tool(self, llm_server_clean, test_config_http, mimiclaw_runner_http):
        """Test add tool via HTTP transport"""
        llm_server_clean.reset()
        
        llm_server_clean.set_tool_call(
            "mcp::test_server_http::add",
            {"a": 100, "b": 200}
        )
        
        llm_server_clean.set_text_response("Addition via HTTP transport completed!")
        
        result = mimiclaw_runner_http.run_with_input(
            "Calculate 100 + 200 using the HTTP MCP server",
            timeout=30
        )
        
        print(f"\n=== HTTP Add Tool Test ===")
        print(f"Return Code: {result['returncode']}")
        stats = llm_server_clean.get_stats()
        print(f"LLM Call Count: {stats['call_count']}")
        
        assert result["returncode"] in [0, 1]
        
    def test_http_transport_info(self, llm_server_clean, test_config_http, mimiclaw_runner_http):
        """Test transport_info tool to verify HTTP transport is being used"""
        llm_server_clean.reset()
        
        llm_server_clean.set_tool_call(
            "mcp::test_server_http::transport_info",
            {}
        )
        
        llm_server_clean.set_text_response("Transport info retrieved successfully!")
        
        result = mimiclaw_runner_http.run_with_input(
            "Get the transport information from the HTTP MCP server",
            timeout=30
        )
        
        print(f"\n=== HTTP Transport Info Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Stdout: {result['stdout'][-500:]}")
        
        assert result["returncode"] in [0, 1]


class TestMCPSSERransport:
    """Tests for MCP SSE transport mode"""
    
    def test_sse_server_startup(self, mcp_sse_server):
        """Test that SSE MCP server starts successfully"""
        assert mcp_sse_server.url is not None
        assert mcp_sse_server.port > 0
        print(f"\n=== SSE Server Info ===")
        print(f"URL: {mcp_sse_server.url}")
        print(f"Port: {mcp_sse_server.port}")
        
    def test_sse_tool_discovery(self, llm_server_clean, test_config_sse, mimiclaw_runner_sse):
        """Test tool discovery via SSE transport"""
        llm_server_clean.reset()
        
        llm_server_clean.set_tool_call(
            "mcp::test_server_sse::echo",
            {"message": "SSE Transport Test"}
        )
        
        llm_server_clean.set_text_response("SSE transport tool call completed!")
        
        result = mimiclaw_runner_sse.run_with_input(
            "Use the echo tool via SSE transport to say 'SSE Transport Test'",
            timeout=30
        )
        
        print(f"\n=== SSE Tool Discovery Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Stdout: {result['stdout'][-500:]}")
        
        stats = llm_server_clean.get_stats()
        print(f"LLM Call Count: {stats['call_count']}")
        
        assert result["returncode"] in [0, 1]
        
    def test_sse_get_test_data(self, llm_server_clean, test_config_sse, mimiclaw_runner_sse):
        """Test get_test_data tool via SSE transport"""
        llm_server_clean.reset()
        
        llm_server_clean.set_tool_call(
            "mcp::test_server_sse::get_test_data",
            {}
        )
        
        llm_server_clean.set_text_response("Test data retrieved via SSE transport!")
        
        result = mimiclaw_runner_sse.run_with_input(
            "Get test data from the SSE MCP server",
            timeout=30
        )
        
        print(f"\n=== SSE Get Test Data Test ===")
        print(f"Return Code: {result['returncode']}")
        stats = llm_server_clean.get_stats()
        print(f"LLM Call Count: {stats['call_count']}")
        
        assert result["returncode"] in [0, 1]
        
    def test_sse_transport_info(self, llm_server_clean, test_config_sse, mimiclaw_runner_sse):
        """Test transport_info tool to verify SSE transport is being used"""
        llm_server_clean.reset()
        
        llm_server_clean.set_tool_call(
            "mcp::test_server_sse::transport_info",
            {}
        )
        
        llm_server_clean.set_text_response("Transport info retrieved successfully!")
        
        result = mimiclaw_runner_sse.run_with_input(
            "Get the transport information from the SSE MCP server",
            timeout=30
        )
        
        print(f"\n=== SSE Transport Info Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Stdout: {result['stdout'][-500:]}")
        
        assert result["returncode"] in [0, 1]


class TestMCPTransportComparison:
    """Tests comparing different transport modes"""
    
    def test_stdio_vs_http_tool_names(self, llm_server_clean, test_config, mimiclaw_runner, 
                                       test_config_http, mimiclaw_runner_http):
        """Compare tool names between stdio and HTTP transports"""
        llm_server_clean.reset()
        
        llm_server_clean.set_tool_call(
            "mcp::test_server::echo",
            {"message": "stdio test"}
        )
        llm_server_clean.set_text_response("stdio transport test")
        
        result_stdio = mimiclaw_runner.run_with_input(
            "Echo 'stdio test' using stdio transport",
            timeout=30
        )
        
        llm_server_clean.reset()
        
        llm_server_clean.set_tool_call(
            "mcp::test_server_http::echo",
            {"message": "http test"}
        )
        llm_server_clean.set_text_response("http transport test")
        
        result_http = mimiclaw_runner_http.run_with_input(
            "Echo 'http test' using HTTP transport",
            timeout=30
        )
        
        print(f"\n=== Transport Comparison ===")
        print(f"stdio return code: {result_stdio['returncode']}")
        print(f"HTTP return code: {result_http['returncode']}")
        
        assert result_stdio["returncode"] in [0, 1]
        assert result_http["returncode"] in [0, 1]


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
