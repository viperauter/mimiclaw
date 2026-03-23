#!/usr/bin/env python3
"""
MCP Integration Test Runner (Python Version)

This script sets up the test environment and runs the MCP integration tests.
It can optionally create a Python virtual environment.

Usage:
    python run_tests.py [options] [pytest_args]

Options:
    --no-venv          Skip virtual environment creation, use system Python
    --clean            Clean up virtual environment before running
    --help             Show this help message

Examples:
    python run_tests.py -v
    python run_tests.py TestEndToEndScenarios
    python run_tests.py --no-venv -x -s
"""

import sys
import os
import argparse
import subprocess
import shutil
import venv
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent.resolve()
PROJECT_ROOT = SCRIPT_DIR.parent.parent
VENV_DIR = SCRIPT_DIR / ".venv"
REQUIREMENTS_FILE = SCRIPT_DIR / "requirements.txt"

class Color:
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    RED = '\033[0;31m'
    NC = '\033[0m'  # No Color

def info(msg):
    print(f"{Color.GREEN}[INFO]{Color.NC} {msg}")

def warn(msg):
    print(f"{Color.YELLOW}[WARN]{Color.NC} {msg}")

def error(msg):
    print(f"{Color.RED}[ERROR]{Color.NC} {msg}")

def find_mimiclaw_binary():
    """Find the mimiclaw binary in common locations."""
    possible_paths = [
        PROJECT_ROOT / "build" / "mimiclaw",
        PROJECT_ROOT / "mimiclaw",
        PROJECT_ROOT / "target" / "debug" / "mimiclaw",
        PROJECT_ROOT / "target" / "release" / "mimiclaw",
    ]
    
    for path in possible_paths:
        if path.exists() and os.access(path, os.X_OK):
            info(f"Found mimiclaw binary: {path}")
            return path
    
    error("Could not find mimiclaw binary. Tried:")
    for path in possible_paths:
        error(f"  - {path}")
    error("Please build mimiclaw first before running tests.")
    return None

def cleanup_processes():
    """Clean up any leftover test processes."""
    info("Cleaning up test processes...")
    import signal
    import psutil
    
    kill_patterns = ["virtual_llm_server", "mimiclaw", "test_mcp"]
    
    for proc in psutil.process_iter(['pid', 'name', 'cmdline']):
        try:
            cmdline = ' '.join(proc.cmdline())
            for pattern in kill_patterns:
                if pattern in cmdline and "run_tests" not in cmdline:
                    info(f"Killing leftover process: {proc.pid} - {cmdline[:50]}...")
                    proc.send_signal(signal.SIGTERM)
                    try:
                        proc.wait(timeout=2)
                    except psutil.TimeoutExpired:
                        proc.kill()
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            pass

def create_virtual_environment(clean=False):
    """Create or update a Python virtual environment."""
    if VENV_DIR.exists():
        if clean:
            info("Cleaning existing virtual environment...")
            shutil.rmtree(VENV_DIR)
        else:
            info("Using existing virtual environment")
            return True
    
    info("Creating Python virtual environment...")
    venv.create(VENV_DIR, with_pip=True)
    
    # Upgrade pip and install requirements
    pip_path = VENV_DIR / "bin" / "pip" if sys.platform != "win32" else VENV_DIR / "Scripts" / "pip.exe"
    
    info("Upgrading pip...")
    subprocess.run([str(pip_path), "install", "--upgrade", "pip"], check=True, capture_output=True)
    
    info("Installing dependencies...")
    subprocess.run([str(pip_path), "install", "-r", str(REQUIREMENTS_FILE)], check=True)
    
    info("Virtual environment created successfully!")
    return True

def check_dependencies():
    """Check if required dependencies are installed."""
    required = ["flask", "requests", "pytest"]
    missing = []
    
    for pkg in required:
        try:
            __import__(pkg)
        except ImportError:
            missing.append(pkg)
    
    if missing:
        error(f"Missing dependencies: {', '.join(missing)}")
        error(f"Please install them with: pip install -r {REQUIREMENTS_FILE}")
        return False
    return True

def main():
    parser = argparse.ArgumentParser(
        description="MCP Integration Test Runner",
        add_help=False,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python run_tests.py -v                          Run all tests with verbose output
  python run_tests.py TestEndToEndScenarios       Run specific test class
  python run_tests.py --no-venv -x -s             Use system Python and stop on first failure
        """
    )
    parser.add_argument("--help", action="help", help="Show this help message")
    parser.add_argument("--no-venv", action="store_true", help="Skip virtual environment creation")
    parser.add_argument("--clean", action="store_true", help="Clean up virtual environment before running")
    
    args, pytest_args = parser.parse_known_args()
    
    info("=" * 40)
    info("    MCP Integration Test Runner")
    info("=" * 40)
    
    # Check for mimiclaw binary
    if not find_mimiclaw_binary():
        return 1
    
    # Cleanup leftover processes
    try:
        cleanup_processes()
    except ImportError:
        warn("psutil not installed, skipping process cleanup")
        # Fallback to pkill
        subprocess.run(
            ["pkill", "-f", "virtual_llm_server|mimiclaw|test_mcp"],
            capture_output=True
        )
    
    # Handle virtual environment
    if not args.no_venv:
        if not create_virtual_environment(args.clean):
            return 1
        
        # Re-launch this script within the virtual environment
        python_path = VENV_DIR / "bin" / "python" if sys.platform != "win32" else VENV_DIR / "Scripts" / "python.exe"
        
        info("Re-launching tests within the virtual environment...")
        cmd = [str(python_path), str(Path(__file__).resolve()), "--no-venv"] + pytest_args
        
        result = subprocess.run(cmd, cwd=SCRIPT_DIR)
        return result.returncode
    else:
        warn("Using system Python (no virtual environment)")
        if not check_dependencies():
            return 1
    
    # Run pytest
    os.chdir(SCRIPT_DIR)
    
    info("Running integration tests...")
    pytest_cmd = [sys.executable, "-m", "pytest", "test_mcp_integration.py", "-v", "--tb=short"]
    
    if pytest_args:
        pytest_cmd = [sys.executable, "-m", "pytest"] + pytest_args
        info(f"Test arguments: {' '.join(pytest_args)}")
    
    result = subprocess.run(pytest_cmd, cwd=SCRIPT_DIR)
    
    # Final cleanup
    try:
        cleanup_processes()
    except:
        pass
    
    info("=" * 40)
    if result.returncode == 0:
        info("✅ All tests passed!")
    else:
        error("❌ Some tests failed!")
    info("=" * 40)
    
    return result.returncode

if __name__ == "__main__":
    sys.exit(main())
