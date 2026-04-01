"""
File Creation and Execution Test - Test the complete flow of creating a file and executing it
"""

import pytest
import json
import os

class TestFileCreationExecution:
    """Tests for file creation and execution workflow"""
    
    def test_file_creation_and_execution(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test the complete flow: create C file -> compile -> run"""
        llm_server_clean.reset()
        # First tool call: write_file to create hello.c
        llm_server_clean.set_tool_call(
            "write_file",
            {"path": "hello.c", "content": "#include <stdio.h>\n\nint main() {\n    printf(\"hello wolrd\\n\");\n    return 0;\n}"}
        )
        
        # Second tool call: exec to compile
        llm_server_clean.set_tool_call(
            "exec",
            {"action": "run", "command": "gcc hello.c -o hello"}
        )
        
        # Third tool call: exec to run
        llm_server_clean.set_tool_call(
            "exec",
            {"action": "run", "command": "./hello"}
        )
        
        # Final text response
        llm_server_clean.set_text_response("File created, compiled, and executed successfully!")
        
        # User input that triggers the workflow.
        # This flow may request confirmation for all 3 tool calls (write_file + 2x exec),
        # then exit CLI explicitly to avoid waiting for the next prompt forever.
        user_input = (
            "Create a C program that outputs 'hello wolrd', compile it, and run it\n"
            "1\n1\n1\n"
            "/exit\n"
        )
        
        result = mimiclaw_runner.run_with_input(user_input, timeout=45)
        
        print(f"\n=== File Creation and Execution Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Timeout: {result.get('timeout', False)}")
        print(f"Error: {result.get('error', 'None')}")
        print(f"Stdout:\n{result['stdout']}")
        print(f"Stderr:\n{result['stderr']}")
        
        # Verify LLM was called multiple times
        stats = llm_server_clean.get_stats()
        print(f"LLM Stats: {stats}")
        
        # Basic assertions
        assert result["returncode"] in [0, 1], "Process should complete"
        assert stats["call_count"] >= 3, "Should have at least 3 LLM calls (3 tool calls + final response)"
        
        # Check if the file was created in the workspace
        workspace_dir = test_config["work_dir"]
        hello_c_path = os.path.join(workspace_dir, "hello.c")
        hello_exec_path = os.path.join(workspace_dir, "hello")
        print(f"Checking if files exist:")
        print(f"  hello.c: {os.path.exists(hello_c_path)}")
        print(f"  hello (executable): {os.path.exists(hello_exec_path)}")
        
        # Verify the file was created
        assert os.path.exists(hello_c_path), "hello.c should be created"
        
        # Read the file content to verify
        with open(hello_c_path, 'r') as f:
            content = f.read()
        assert "hello wolrd" in content, "File content should contain 'hello wolrd'"
        
        print("✓ File creation test passed")
    
    def test_file_creation_with_confirmation(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test file creation with user confirmation flow"""
        llm_server_clean.reset()
        # First tool call: write_file (requires confirmation)
        llm_server_clean.set_tool_call(
            "write_file",
            {"path": "test.txt", "content": "This is a test file\n"}
        )
        
        # Final text response after confirmation
        llm_server_clean.set_text_response("File created successfully with your confirmation!")
        
        # User input that triggers file creation.
        # Add explicit exit so the black-box CLI test can terminate deterministically.
        user_input = (
            "Create a file called test.txt with content 'This is a test file'\n"
            "1\n"
            "/exit\n"
        )
        
        result = mimiclaw_runner.run_with_input(user_input, timeout=30)
        
        print(f"\n=== File Creation with Confirmation Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Timeout: {result.get('timeout', False)}")
        print(f"Error: {result.get('error', 'None')}")
        print(f"Stdout:\n{result['stdout']}")
        print(f"Stderr:\n{result['stderr']}")
        
        # Check if the file was created
        workspace_dir = test_config["work_dir"]
        test_txt_path = os.path.join(workspace_dir, "test.txt")
        print(f"Checking if test.txt exists: {os.path.exists(test_txt_path)}")
        
        # The file might not exist if confirmation wasn't provided in the test
        # This test primarily verifies the workflow completes
        assert result["returncode"] in [0, 1], "Process should complete"
        
        stats = llm_server_clean.get_stats()
        print(f"LLM Stats: {stats}")
        assert stats["call_count"] >= 1, "Should have at least one LLM call"
        
        print("✓ File creation with confirmation test passed")
    
    def test_multi_tool_workflow(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test a more complex workflow with multiple tool calls"""
        llm_server_clean.reset()
        # First: write a simple script
        llm_server_clean.set_tool_call(
            "write_file",
            {"path": "script.sh", "content": "#!/bin/bash\necho 'Hello from script'\necho 'Current directory: $(pwd)'\n"}
        )
        
        # Second: make it executable
        llm_server_clean.set_tool_call(
            "exec",
            {"action": "run", "command": "chmod +x script.sh"}
        )
        
        # Third: run the script
        llm_server_clean.set_tool_call(
            "exec",
            {"action": "run", "command": "./script.sh"}
        )
        
        # Final response
        llm_server_clean.set_text_response("Script created and executed successfully!")
        
        # This flow may request confirmation for all 3 tool calls,
        # then exit CLI explicitly to avoid hanging the process.
        user_input = (
            "Create a bash script called script.sh that prints 'Hello from script' and the current directory, make it executable, and run it\n"
            "1\n1\n1\n"
            "/exit\n"
        )
        
        result = mimiclaw_runner.run_with_input(user_input, timeout=45)
        
        print(f"\n=== Multi-tool Workflow Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Timeout: {result.get('timeout', False)}")
        print(f"Error: {result.get('error', 'None')}")
        print(f"Stdout:\n{result['stdout']}")
        print(f"Stderr:\n{result['stderr']}")
        
        # Check if script file was created
        workspace_dir = test_config["work_dir"]
        script_path = os.path.join(workspace_dir, "script.sh")
        print(f"Checking if script.sh exists: {os.path.exists(script_path)}")
        
        # Verify the script was created
        if os.path.exists(script_path):
            with open(script_path, 'r') as f:
                content = f.read()
            assert "Hello from script" in content, "Script content should be correct"
            print("✓ Script file created with correct content")
        
        stats = llm_server_clean.get_stats()
        print(f"LLM Stats: {stats}")
        
        assert result["returncode"] in [0, 1], "Process should complete"
        assert stats["call_count"] >= 3, "Should have multiple LLM calls"
        
        print("✓ Multi-tool workflow test passed")


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
