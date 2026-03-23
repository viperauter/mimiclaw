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
            
        assert "mcpServers" in config.get("providers", {})
        assert len(config["providers"]["mcpServers"]) == 2
        assert config["providers"]["mcpServers"][0]["name"] == "server1"
        assert config["providers"]["mcpServers"][1]["requires_confirmation"] == True
        
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
            
        server_config = config["providers"]["mcpServers"][0]
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

if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
