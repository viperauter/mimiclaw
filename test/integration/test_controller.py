#!/usr/bin/env python3
"""
Test Controller - Manages test services and configuration
"""

import subprocess
import time
import json
import os
import sys
import argparse
import requests

class TestController:
    def __init__(self, base_dir):
        self.base_dir = base_dir
        self.integration_dir = os.path.join(base_dir, "test", "integration")
        self.config_dir = os.path.join(self.integration_dir, "config")
        os.makedirs(self.config_dir, exist_ok=True)
        
        self.llm_server_proc = None
        self.llm_port = 9999
        
    def start_llm_server(self):
        """Start the virtual LLM server"""
        server_script = os.path.join(self.integration_dir, "virtual_llm_server.py")
        cmd = [sys.executable, server_script, "--port", str(self.llm_port)]
        
        self.llm_server_proc = subprocess.Popen(
            cmd,
            cwd=self.base_dir,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        
        # Wait for server to start
        for i in range(10):
            try:
                response = requests.get(f"http://localhost:{self.llm_port}/v1/models", timeout=1)
                if response.status_code == 200:
                    print(f"✅ LLM Server started on port {self.llm_port}")
                    return True
            except requests.exceptions.RequestException:
                time.sleep(0.5)
                
        print("❌ Failed to start LLM Server")
        self.stop_all()
        return False
        
    def stop_all(self):
        """Stop all services"""
        if self.llm_server_proc:
            self.llm_server_proc.terminate()
            try:
                self.llm_server_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.llm_server_proc.kill()
            print("✅ LLM Server stopped")
            
    def generate_config(self):
        """Generate test configuration file"""
        config_path = os.path.join(self.config_dir, "test_config.json")
        
        mcp_server_script = os.path.join(self.integration_dir, "test_mcp_server.py")
        python_path = sys.executable
        
        config = {
            "schemaVersion": 3,
            "providers": {
                "test_provider": {
                    "apiKey": "test_key_12345",
                    "apiBase": f"http://localhost:{self.llm_port}/v1",
                    "model": "virtual-llm"
                },
                "mcpServers": [
                    {
                        "name": "test_server",
                        "command": f"{python_path} {mcp_server_script}",
                        "requires_confirmation": False
                    }
                ]
            },
            "agents": {
                "defaults": {
                    "workspace": self.config_dir,
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
            
        print(f"✅ Config generated: {config_path}")
        return config_path
        
    def set_llm_mode(self, mode):
        """Set LLM response mode"""
        try:
            response = requests.post(
                f"http://localhost:{self.llm_port}/test/set_mode",
                json={"mode": mode},
                timeout=2
            )
            return response.status_code == 200
        except:
            return False
            
    def get_llm_stats(self):
        """Get LLM statistics"""
        try:
            response = requests.get(
                f"http://localhost:{self.llm_port}/test/stats",
                timeout=2
            )
            return response.json()
        except:
            return None

def main():
    parser = argparse.ArgumentParser(description="Test Controller")
    parser.add_argument("--action", choices=["start", "stop", "config"], default="start", 
                       help="Action to perform")
    parser.add_argument("--base-dir", default=os.getcwd(), help="Base directory")
    
    args = parser.parse_args()
    
    controller = TestController(args.base_dir)
    
    if args.action == "start":
        if controller.start_llm_server():
            controller.generate_config()
            print("\n✅ Test environment ready!")
            print(f"   Config: {os.path.join(controller.config_dir, 'test_config.json')}")
            print("\nPress Ctrl+C to stop...")
            try:
                while True:
                    time.sleep(1)
            except KeyboardInterrupt:
                controller.stop_all()
                
    elif args.action == "config":
        controller.generate_config()
        
    elif args.action == "stop":
        controller.stop_all()

if __name__ == "__main__":
    main()
