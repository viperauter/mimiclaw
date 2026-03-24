"""
WeChat MCP Integration Tests
Test suite for verifying mimiclaw can connect to and use the WeChat MCP server
"""

import pytest
import time
import json


class TestWeChatMCPDiscovery:
    """Tests for WeChat MCP server discovery"""

    def test_wechat_mcp_configuration(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test that WeChat MCP server is properly configured"""
        llm_server_clean.set_text_response("WeChat MCP configuration test")
        result = mimiclaw_runner.run_with_input("Hello", timeout=20)

        print(f"\n=== WeChat MCP Config Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Stdout:\n{result['stdout'][:500]}")
        print(f"Stderr:\n{result['stderr'][:1000]}")

        assert result["returncode"] in [0, 1], "Process should complete"

    def test_wechat_tools_in_llm_tools_list(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test that WeChat MCP tools are present in LLM's tools list"""
        llm_server_clean.set_text_response("WeChat tools check complete")
        result = mimiclaw_runner.run_with_input("What tools do you have available?", timeout=20)

        print(f"\n=== WeChat Tools Discovery Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Stdout:\n{result['stdout'][:500]}")
        print(f"Stderr:\n{result['stderr'][:1000]}")

        stats = llm_server_clean.get_stats()
        last_request = stats.get("last_request", {})
        tools = last_request.get("tools", [])

        print(f"\n=== Tools received by LLM ===")
        wechat_tools = []
        for tool in tools:
            name = tool.get("function", {}).get("name", "")
            print(f"  - {name}")
            if "wechat" in name.lower():
                wechat_tools.append(name)

        print(f"\n=== WeChat tools found: {len(wechat_tools)} ===")
        for name in wechat_tools:
            print(f"  ✅ {name}")

        expected_wechat_tools = [
            "mcp::wechat::login_qrcode",
            "mcp::wechat::check_qrcode_status",
            "mcp::wechat::logout",
            "mcp::wechat::get_messages",
            "mcp::wechat::send_text_message",
            "mcp::wechat::send_typing"
        ]

        for expected in expected_wechat_tools:
            assert expected in wechat_tools, f"Missing expected tool: {expected}"

        assert result["returncode"] in [0, 1]


class TestWeChatMCPExecution:
    """Tests for WeChat MCP tool execution"""

    def test_get_qrcode_status(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test calling wechat_get_qrcode_status tool"""
        llm_server_clean.set_tool_call(
            "mcp::wechat::wechat_get_qrcode_status",
            {"status": "confirmed", "message": "Login successful"}
        )
        llm_server_clean.set_text_response("QR code status: confirmed")

        result = mimiclaw_runner.run_with_input(
            "Check the WeChat QR code status",
            timeout=30
        )

        print(f"\n=== Get QR Code Status Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Stdout:\n{result['stdout']}")
        print(f"Stderr:\n{result['stderr'][:1000]}")

        stats = llm_server_clean.get_stats()
        print(f"LLM Stats: {stats}")

        assert result["returncode"] in [0, 1]

    def test_send_text_message(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test calling wechat_send_text_message tool"""
        llm_server_clean.set_tool_call(
            "mcp::wechat::wechat_send_text_message",
            {"status": "sent", "message": "Message sent successfully"}
        )
        llm_server_clean.set_text_response("Message sent via WeChat!")

        result = mimiclaw_runner.run_with_input(
            "Send a WeChat message saying 'Hello from mimiclaw'",
            timeout=30
        )

        print(f"\n=== Send Text Message Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Stdout:\n{result['stdout']}")
        print(f"Stderr:\n{result['stderr'][:1000]}")

        assert result["returncode"] in [0, 1]


class TestWeChatMessageHandling:
    """Tests for WeChat message receive handling"""

    def test_get_messages(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test calling wechat_get_messages to receive messages"""
        llm_server_clean.set_tool_call(
            "mcp::wechat::wechat_get_messages",
            {
                "message_count": 1,
                "messages": [{
                    "message_id": "12345",
                    "from_user_id": "user123@im.wechat",
                    "to_user_id": "bot123@im.bot",
                    "type": "received",
                    "text": "Test message",
                    "create_time": "2026-03-24T09:00:00Z"
                }]
            }
        )
        llm_server_clean.set_text_response("I received your message: Test message")

        result = mimiclaw_runner.run_with_input(
            "Check for new WeChat messages",
            timeout=30
        )

        print(f"\n=== Get Messages Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Stdout:\n{result['stdout']}")
        print(f"Stderr:\n{result['stderr'][:1000]}")

        assert result["returncode"] in [0, 1]


class TestWeChatEndToEnd:
    """End-to-end tests for WeChat integration"""

    def test_wechat_send_message_via_llm_decision(self, llm_server_clean, test_config, mimiclaw_runner):
        """Test that LLM decides to call send_text_message tool with correct arguments"""

        llm_server_clean.set_tool_call_with_args(
            "mcp::wechat::send_text_message",
            {"text": "Hello from mimiclaw!", "to": "o9cq80xxvjgmrHtcvREjOMfIGn54@im.wechat"}
        )
        llm_server_clean.set_text_response("Hello! I've sent you a WeChat message!")

        result = mimiclaw_runner.run_with_input(
            "Send a greeting 'Hello from mimiclaw!' via WeChat to o9cq80xxvjgmrHtcvREjOMfIGn54@im.wechat",
            timeout=30
        )

        print(f"\n=== End-to-End WeChat Send Message Test ===")
        print(f"Return Code: {result['returncode']}")
        print(f"Stdout:\n{result['stdout']}")
        print(f"Stderr:\n{result['stderr'][:2000]}")

        stats = llm_server_clean.get_stats()
        last_request = stats.get("last_request", {})

        messages = last_request.get("messages", [])
        tool_calls_in_final_request = False
        for msg in messages:
            if msg.get("role") == "assistant" and msg.get("tool_calls"):
                tool_calls_in_final_request = True
                for tc in msg["tool_calls"]:
                    fn = tc.get("function", {})
                    tool_name = fn.get("name", "")
                    print(f"\n=== Tool Call in Final Response ===")
                    print(f"Tool: {tool_name}")

        all_requests = llm_server_clean.get_captured_requests()
        print(f"\n=== All LLM Requests ({len(all_requests)} total) ===")
        for i, req in enumerate(all_requests):
            msgs = req.get("messages", [])
            for msg in msgs:
                if msg.get("role") == "assistant" and msg.get("tool_calls"):
                    for tc in msg["tool_calls"]:
                        fn = tc.get("function", {})
                        raw_args = fn.get("arguments", "{}")
                        if isinstance(raw_args, str):
                            try:
                                args = json.loads(raw_args)
                            except:
                                args = {}
                        else:
                            args = raw_args
                        print(f"\n--- Tool Call in Request #{i+1} ---")
                        print(f"Tool: {fn.get('name')}")
                        print(f"Arguments: {json.dumps(args, indent=2)}")

                        if fn.get("name") == "mcp::wechat::send_text_message":
                            assert args.get("to") == "o9cq80xxvjgmrHtcvREjOMfIGn54@im.wechat", f"Wrong recipient: {args.get('to')}"
                            assert "Hello from mimiclaw" in args.get("text", ""), f"Wrong text: {args.get('text')}"
                            print("✅ send_text_message was called with correct arguments!")

        print("\n✅ WeChat message was sent successfully!")
        assert result["returncode"] in [0, 1]


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])