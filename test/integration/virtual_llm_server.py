#!/usr/bin/env python3
"""
Virtual LLM Server - OpenAI API compatible mock server for integration testing
Provides dynamic response configuration via REST API
"""

from flask import Flask, request, jsonify
import json
import time
import argparse
import threading
from collections import deque

app = Flask(__name__)

# Response configuration storage
class LLMState:
    def __init__(self):
        self.reset()
        
    def reset(self):
        self.call_count = 0
        self.custom_responses = deque()
        self.current_mode = "default"
        self.last_request = None
        self.captured_requests = []
        
    def add_custom_response(self, response):
        """Add a custom response to the queue"""
        self.custom_responses.append(response)
        
    def get_next_response(self):
        """Get next response from queue or default"""
        if self.custom_responses:
            return self.custom_responses.popleft()
        return self._get_default_response()
        
    def _get_default_response(self):
        """Default response when no custom response is set"""
        return {
            "role": "assistant",
            "content": "Hello! This is a default response from Virtual LLM Server."
        }

_llm_state = LLMState()

@app.route("/v1/chat/completions", methods=["POST"])
def chat_completions():
    """
    Mock OpenAI chat.completions API endpoint
    """
    _llm_state.call_count += 1
    
    request_data = request.json
    _llm_state.last_request = request_data
    _llm_state.captured_requests.append(request_data.copy())
    
    print(f"\n=== LLM Request #{_llm_state.call_count} ===")
    print(f"Model: {request_data.get('model')}")
    print(f"Messages count: {len(request_data.get('messages', []))}")
    
    # Check if there's a tool response in conversation
    messages = request_data.get('messages', [])
    has_tool_response = any(msg.get('role') == 'tool' for msg in messages)
    
    response_template = _llm_state.get_next_response()
    response = _build_completion_response(request_data, response_template, has_tool_response)
    
    print(f"Response type: {'tool_call' if response.get('choices', [{}])[0].get('message', {}).get('tool_calls') else 'text'}")
    return jsonify(response)

@app.route("/v1/models", methods=["GET"])
def list_models():
    """Return available models"""
    return jsonify({
        "object": "list",
        "data": [{"id": "virtual-llm", "object": "model", "owned_by": "test"}]
    })

@app.route("/test/response/queue", methods=["POST"])
def queue_response():
    """
    Queue a custom response
    Request body: {
        "response": { ... response template ... },
        "count": 1  // optional, how many times to repeat this response
    }
    """
    data = request.json
    response = data.get("response")
    count = data.get("count", 1)
    
    if not response:
        return jsonify({"status": "error", "message": "Missing 'response' field"}), 400
        
    for _ in range(count):
        _llm_state.add_custom_response(response)
    
    return jsonify({
        "status": "ok", 
        "queue_size": len(_llm_state.custom_responses)
    })

@app.route("/test/response/single", methods=["POST"])
def set_single_response():
    """
    Set a single response (clears queue first)
    """
    data = request.json
    response = data.get("response")
    
    if not response:
        return jsonify({"status": "error", "message": "Missing 'response' field"}), 400
        
    _llm_state.custom_responses.clear()
    _llm_state.add_custom_response(response)
    
    return jsonify({"status": "ok"})

@app.route("/test/tool_call", methods=["POST"])
def set_tool_call_response():
    """
    Convenience endpoint to set a tool call response
    Request body: {
        "tool_name": "mcp::test_server::echo",
        "arguments": {"message": "test"},
        "count": 1
    }
    """
    data = request.json
    tool_name = data.get("tool_name", "mcp::test_server::echo")
    arguments = data.get("arguments", {})
    count = data.get("count", 1)
    
    response = {
        "role": "assistant",
        "content": None,
        "tool_calls": [{
            "id": f"call_mock_{int(time.time())}",
            "type": "function",
            "function": {
                "name": tool_name,
                "arguments": json.dumps(arguments)
            }
        }]
    }
    
    for _ in range(count):
        _llm_state.add_custom_response(response)
    
    return jsonify({"status": "ok", "tool": tool_name})

@app.route("/test/reset", methods=["POST"])
def reset_state():
    """Reset all server state"""
    try:
        clear_history = request.json.get("clear_history", True) if request.is_json else True
    except:
        clear_history = True
    _llm_state.reset()
    return jsonify({"status": "ok"})

@app.route("/test/stats", methods=["GET"])
def get_stats():
    """Get test statistics"""
    return jsonify({
        "call_count": _llm_state.call_count,
        "queue_size": len(_llm_state.custom_responses),
        "last_request": _llm_state.last_request
    })

@app.route("/test/requests", methods=["GET"])
def get_requests():
    """Get captured requests"""
    return jsonify({
        "requests": _llm_state.captured_requests
    })

@app.route("/test/requests/last", methods=["GET"])
def get_last_request():
    """Get last captured request"""
    return jsonify(_llm_state.last_request if _llm_state.last_request else {})

def _build_completion_response(request_data, message_template, has_tool_response=False):
    """Build a complete completion response"""
    base_response = {
        "id": f"virtual-llm-{int(time.time())}-{_llm_state.call_count}",
        "object": "chat.completion",
        "created": int(time.time()),
        "model": request_data.get('model', 'virtual-llm'),
        "usage": {"prompt_tokens": 10, "completion_tokens": 20, "total_tokens": 30},
        "choices": []
    }
    
    # If this is a follow-up after tool response, send completion message
    if has_tool_response and not message_template.get("tool_calls"):
        final_message = {
            "role": "assistant",
            "content": "Task completed successfully! I've processed the tool response."
        }
    else:
        final_message = message_template
    
    base_response["choices"].append({
        "index": 0,
        "message": final_message,
        "finish_reason": "tool_calls" if final_message.get("tool_calls") else "stop"
    })
    
    return base_response

def run_server(host="127.0.0.1", port=9999, debug=False):
    """Run the server"""
    print(f"🚀 Virtual LLM Server starting on http://{host}:{port}")
    print(f"   Test endpoints available:")
    print(f"   - POST /test/response/queue - Queue custom responses")
    print(f"   - POST /test/tool_call - Set tool call response")
    print(f"   - POST /test/reset - Reset server state")
    print(f"   - GET /test/stats - Get server statistics")
    app.run(host=host, port=port, debug=debug, threaded=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Virtual LLM Server")
    parser.add_argument("--port", type=int, default=9999, help="Port to listen on")
    parser.add_argument("--host", type=str, default="127.0.0.1", help="Host to bind")
    parser.add_argument("--debug", action="store_true", help="Enable debug mode")
    args = parser.parse_args()
    
    run_server(args.host, args.port, args.debug)
