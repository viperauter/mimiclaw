"""
pytest configuration and fixtures for MCP integration testing
"""

import pytest
import os
import sys
import json
import tempfile
import subprocess
import time
import requests

# Add the integration directory to the path
sys.path.insert(0, os.path.dirname(__file__))

class LLMServerFixture:
    """Fixture for controlling the virtual LLM server"""
    
    def __init__(self, port=None):
        self.port = port or self._find_free_port()
        self.base_url = f"http://localhost:{self.port}"
        self._process = None
        
    def _find_free_port(self):
        """Find a free port to use"""
        import socket
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.bind(('', 0))
        port = sock.getsockname()[1]
        sock.close()
        return port
        
    def start(self):
        """Start the LLM server"""
        server_script = os.path.join(os.path.dirname(__file__), "virtual_llm_server.py")
        self._process = subprocess.Popen(
            [sys.executable, server_script, "--port", str(self.port)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        
        # Wait for server to be ready
        for i in range(20):
            try:
                response = requests.get(f"{self.base_url}/v1/models", timeout=1)
                if response.status_code == 200:
                    time.sleep(0.2)
                    return True
            except requests.exceptions.RequestException:
                time.sleep(0.3)
                
        raise RuntimeError("Failed to start LLM server")
        
    def stop(self):
        """Stop the LLM server"""
        if self._process:
            self._process.terminate()
            try:
                self._process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self._process.kill()
            self._process = None
            
    def reset(self):
        """Reset server state"""
        # Send empty JSON to avoid Flask 415 Unsupported Media Type error
        requests.post(f"{self.base_url}/test/reset", json={}, timeout=2)
        
    def set_tool_call(self, tool_name, arguments=None, count=1):
        """Set the LLM to respond with a tool call"""
        response = requests.post(
            f"{self.base_url}/test/tool_call",
            json={
                "tool_name": tool_name,
                "arguments": arguments or {},
                "count": count
            },
            timeout=2
        )
        response.raise_for_status()
        
    def set_custom_response(self, response, count=1):
        """Set a custom response"""
        for _ in range(count):
            response = requests.post(
                f"{self.base_url}/test/response/queue",
                json={"response": response},
                timeout=2
            )
            response.raise_for_status()
            
    def set_text_response(self, content, count=1):
        """Set a text response (no tool call)"""
        response = {"role": "assistant", "content": content}
        for _ in range(count):
            requests.post(
                f"{self.base_url}/test/response/queue",
                json={"response": response},
                timeout=2
            ).raise_for_status()
            
    def get_stats(self):
        """Get server statistics"""
        response = requests.get(f"{self.base_url}/test/stats", timeout=2)
        return response.json()
        
    def get_last_request(self):
        """Get last captured request"""
        response = requests.get(f"{self.base_url}/test/requests/last", timeout=2)
        return response.json()

class TestConfigGenerator:
    """Generate test configuration files"""
    
    def __init__(self, work_dir=None):
        self.work_dir = work_dir or tempfile.mkdtemp(prefix="mcp_test_")
        os.makedirs(self.work_dir, exist_ok=True)
        
    def generate_config(self, mcp_servers=None, llm_port=9999):
        """Generate config.json with MCP servers configured"""
        config_path = os.path.join(self.work_dir, "config.json")
        
        python_path = sys.executable
        mcp_script = os.path.join(os.path.dirname(__file__), "test_mcp_server.py")
        
        # Default MCP server config
        if mcp_servers is None:
            mcp_servers = [
                {
                    "name": "test_server",
                    "command": f"{python_path} {mcp_script}",
                    "requires_confirmation": False
                }
            ]
        
        config = {
            "schemaVersion": 3,
            "providers": {
                "test_provider": {
                    "apiKey": "test_key_12345",
                    "apiBase": f"http://localhost:{llm_port}/v1",
                    "model": "virtual-llm"
                },
                "mcpServers": mcp_servers
            },
            "agents": {
                "defaults": {
                    "workspace": self.work_dir,
                    "timezone": "UTC",
                    "model": "virtual-llm",
                    "provider": "test_provider",
                    "apiProtocol": "openai",
                    "maxTokens": 1000,
                    "temperature": 0.7,
                    "maxToolIterations": 5,
                    "tools": ["*"]
                }
            },
            "logging": {
                "level": "DEBUG"
            }
        }
        
        with open(config_path, "w") as f:
            json.dump(config, f, indent=2)
            
        return config_path
        
    def cleanup(self):
        """Clean up temporary directory"""
        import shutil
        shutil.rmtree(self.work_dir, ignore_errors=True)

@pytest.fixture(scope="session")
def llm_server():
    """Session-scoped LLM server fixture"""
    server = LLMServerFixture()
    try:
        server.start()
        yield server
    finally:
        server.stop()

@pytest.fixture
def llm_server_clean(llm_server):
    """Function-scoped LLM server with clean state"""
    llm_server.reset()
    return llm_server

@pytest.fixture
def test_config(llm_server):
    """Function-scoped test configuration with correct LLM server port"""
    config_gen = TestConfigGenerator()
    try:
        # Use the actual port from the running LLM server
        config_path = config_gen.generate_config(llm_port=llm_server.port)
        yield {
            "config_path": config_path,
            "work_dir": config_gen.work_dir,
            "config_gen": config_gen,
            "llm_port": llm_server.port
        }
    finally:
        config_gen.cleanup()

class MimiclawProcess:
    """Wrapper for running mimiclaw process"""
    
    def __init__(self, config_path):
        self.config_path = config_path
        self._process = None
        
    def run_with_input(self, user_input, timeout=30):
        """Run mimiclaw with user input and capture output"""
        mimiclaw_bin = self._find_mimiclaw_binary()
        
        # Command: mimiclaw [config_file] - input is provided via stdin
        cmd = [mimiclaw_bin, self.config_path]
        
        env = os.environ.copy()
        env["PYTHONUNBUFFERED"] = "1"
        
        # Ensure input ends with newline - CLI requires newline to process input
        if not user_input.endswith('\n'):
            user_input = user_input + '\n'
        
        try:
            result = subprocess.run(
                cmd,
                input=user_input,
                capture_output=True,
                text=True,
                timeout=timeout,
                env=env
            )
            return {
                "stdout": result.stdout,
                "stderr": result.stderr,
                "returncode": result.returncode
            }
        except subprocess.TimeoutExpired as e:
            return {
                "stdout": e.stdout.decode() if e.stdout else "",
                "stderr": e.stderr.decode() if e.stderr else "",
                "returncode": -1,
                "timeout": True
            }
            
    def _find_mimiclaw_binary(self):
        """Find the mimiclaw executable"""
        # Get project root (test/integration -> project root)
        project_root = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
        
        # Try common locations
        build_locations = [
            os.path.join(project_root, "build/mimiclaw"),
            os.path.join(project_root, "build/bin/mimiclaw"),
            os.path.join(project_root, "cmake-build-debug/mimiclaw"),
            os.path.join(project_root, "cmake-build-release/mimiclaw"),
            os.path.join(project_root, "bin/mimiclaw"),
        ]

        for loc in build_locations:
            if os.path.exists(loc) and os.access(loc, os.X_OK):
                return loc
                
        # Try in PATH
        import shutil
        mimiclaw_path = shutil.which("mimiclaw")
        if mimiclaw_path:
            return mimiclaw_path
            
        raise RuntimeError(
            "Could not find mimiclaw executable. "
            "Please build mimiclaw first and ensure it's in the expected location."
        )

@pytest.fixture
def mimiclaw_runner(test_config):
    """Fixture to run mimiclaw with test config"""
    return MimiclawProcess(test_config["config_path"])
