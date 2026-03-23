#!/usr/bin/env python3
"""
MCP Integration Test Runner
Orchestrates the complete integration test environment
"""

import subprocess
import sys
import os
import time
import json
import argparse

# Add the integration directory to path
sys.path.insert(0, os.path.dirname(__file__))

def check_dependencies():
    """Check required dependencies"""
    required_modules = ["flask", "requests", "pytest"]
    missing = []
    
    for module in required_modules:
        try:
            __import__(module)
        except ImportError:
            missing.append(module)
    
    if missing:
        print(f"❌ Missing required modules: {', '.join(missing)}")
        print(f"   Install with: pip install {' '.join(missing)}")
        return False
    return True

def check_mimiclaw_binary():
    """Check if mimiclaw binary exists"""
    locations = [
        "./build/mimiclaw",
        "./build/bin/mimiclaw",
        "./cmake-build-debug/mimiclaw",
        "./cmake-build-release/mimiclaw",
        "./bin/mimiclaw",
    ]
    
    for loc in locations:
        if os.path.exists(loc) and os.access(loc, os.X_OK):
            return os.path.abspath(loc)
    
    return None

def build_mimiclaw():
    """Attempt to build mimiclaw"""
    print("🔨 Building mimiclaw...")
    
    # Try cmake build
    if os.path.exists("CMakeLists.txt"):
        cmake_cmd = [
            "cmake", "-B", "build", 
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DMIMI_ENABLE_SUBAGENT=OFF"
        ]
        make_cmd = ["cmake", "--build", "build", "-j4"]
        
        try:
            subprocess.run(cmake_cmd, check=True, capture_output=True, text=True)
            subprocess.run(make_cmd, check=True, capture_output=True, text=True)
            print("✅ Build successful")
            return True
        except subprocess.CalledProcessError as e:
            print(f"❌ Build failed: {e.stderr}")
            return False
    
    print("❌ No CMakeLists.txt found")
    return False

def generate_test_config(output_dir, llm_port=9999):
    """Generate test configuration"""
    work_dir = os.path.abspath(output_dir)
    os.makedirs(work_dir, exist_ok=True)
    
    python_path = sys.executable
    mcp_script = os.path.join(os.path.dirname(__file__), "test_mcp_server.py")
    
    config = {
        "schemaVersion": 3,
        "providers": {
            "test_provider": {
                "apiKey": "test_key_12345",
                "apiBase": f"http://localhost:{llm_port}/v1",
                "model": "virtual-llm"
            }
        },
        "agents": {
            "defaults": {
                "workspace": work_dir,
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
        "providers": {
            "mcpServers": [
                {
                    "name": "test_server",
                    "command": f"{python_path} {mcp_script}",
                    "requires_confirmation": False
                }
            ]
        },
        "logging": {
            "level": "DEBUG"
        }
    }
    
    config_path = os.path.join(work_dir, "config.json")
    with open(config_path, "w") as f:
        json.dump(config, f, indent=2)
    
    return config_path

def start_llm_server(port=9999):
    """Start the virtual LLM server"""
    script = os.path.join(os.path.dirname(__file__), "virtual_llm_server.py")
    proc = subprocess.Popen(
        [sys.executable, script, "--port", str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    # Wait for server to be ready
    import requests
    for i in range(20):
        try:
            response = requests.get(f"http://localhost:{port}/v1/models", timeout=1)
            if response.status_code == 200:
                time.sleep(0.2)
                print(f"✅ LLM Server started on port {port}")
                return proc
        except:
            time.sleep(0.3)
    
    proc.terminate()
    raise RuntimeError("Failed to start LLM server")

def run_pytest_tests():
    """Run pytest integration tests"""
    print("\n🧪 Running integration tests...")
    cmd = [
        sys.executable, "-m", "pytest",
        os.path.join(os.path.dirname(__file__), "test_mcp_integration.py"),
        "-v", "-s", "--timeout=120"
    ]
    
    result = subprocess.run(cmd, cwd=os.path.dirname(__file__))
    return result.returncode

def run_manual_test(config_path):
    """Run a quick manual test"""
    print("\n🔍 Running manual verification test...")
    
    mimiclaw_bin = check_mimiclaw_binary()
    if not mimiclaw_bin:
        print("❌ mimiclaw binary not found")
        return 1
    
    # First, send a request to configure LLM server
    import requests
    try:
        requests.post(
            "http://localhost:9999/test/tool_call",
            json={
                "tool_name": "mcp::test_server::echo",
                "arguments": {"message": "Manual Test"},
                "count": 1
            },
            timeout=2
        )
        requests.post(
            "http://localhost:9999/test/response/queue",
            json={"response": {"role": "assistant", "content": "Test complete!"}},
            timeout=2
        )
    except:
        print("⚠️  Could not configure LLM server")
    
    cmd = [mimiclaw_bin, "-c", config_path, "-p", "Hello, please run a quick test!"]
    print(f"Running: {' '.join(cmd)}")
    
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    print(f"\nReturn Code: {result.returncode}")
    print(f"Stdout:\n{result.stdout}")
    print(f"\nStderr (last 20 lines):\n")
    print('\n'.join(result.stderr.split('\n')[-20:]))
    
    return result.returncode

def main():
    parser = argparse.ArgumentParser(description="MCP Integration Test Runner")
    parser.add_argument("--mode", choices=["test", "manual", "server"], 
                       default="test", help="Run mode")
    parser.add_argument("--config-only", action="store_true", 
                       help="Only generate config file")
    parser.add_argument("--output-dir", default="/tmp/mcp_test", 
                       help="Output directory for config and workspace")
    parser.add_argument("--llm-port", type=int, default=9999,
                       help="Port for virtual LLM server")
    args = parser.parse_args()
    
    print("=" * 60)
    print("MCP Integration Test Suite")
    print("=" * 60)
    
    # Check dependencies
    if not check_dependencies():
        return 1
    
    # Build if needed
    if not check_mimiclaw_binary():
        print("⚠️  mimiclaw binary not found, attempting to build...")
        if not build_mimiclaw():
            print("❌ Could not build mimiclaw")
            return 1
    
    print(f"✅ mimiclaw binary found at: {check_mimiclaw_binary()}")
    
    # Generate config
    config_path = generate_test_config(args.output_dir, args.llm_port)
    print(f"✅ Test config generated: {config_path}")
    
    if args.config_only:
        print(f"\n📝 Config file generated at: {config_path}")
        return 0
    
    if args.mode == "server":
        # Just start servers
        try:
            llm_proc = start_llm_server(args.llm_port)
            print(f"\n🚀 Test environment ready!")
            print(f"   LLM Server: http://localhost:{args.llm_port}")
            print(f"   Config: {config_path}")
            print("\nPress Ctrl+C to stop servers...")
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            print("\n🛑 Stopping servers...")
            if 'llm_proc' in locals():
                llm_proc.terminate()
        return 0
    
    elif args.mode == "manual":
        try:
            llm_proc = start_llm_server(args.llm_port)
            return run_manual_test(config_path)
        finally:
            if 'llm_proc' in locals():
                llm_proc.terminate()
    
    else:  # test mode
        try:
            llm_proc = start_llm_server(args.llm_port)
            return run_pytest_tests()
        finally:
            if 'llm_proc' in locals():
                llm_proc.terminate()

if __name__ == "__main__":
    sys.exit(main())
