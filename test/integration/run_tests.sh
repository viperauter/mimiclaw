#!/bin/bash
#
# MCP Integration Test Runner
#
# This script sets up a Python virtual environment and runs the MCP integration tests.
#
# Usage:
#   ./run_tests.sh [options] [pytest_args]
#
# Options:
#   --no-venv          Skip virtual environment creation, use system Python
#   --clean            Clean up virtual environment before running
#   --help             Show this help message
#
# Examples:
#   ./run_tests.sh -v                          Run all tests with verbose output
#   ./run_tests.sh TestEndToEndScenarios       Run specific test class
#   ./run_tests.sh --no-venv -x -s             Use system Python and stop on first failure
#

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
VENV_DIR="${SCRIPT_DIR}/.venv"
REQUIREMENTS_FILE="${SCRIPT_DIR}/requirements.txt"

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

show_help() {
    grep -E '^# Usage:|^# Options:|^# Examples:|^#   ' "$0" | sed 's/^# //'
}

check_mimiclaw_binary() {
    local possible_paths=(
        "${PROJECT_ROOT}/build/mimiclaw"
        "${PROJECT_ROOT}/mimiclaw"
        "${PROJECT_ROOT}/target/debug/mimiclaw"
        "${PROJECT_ROOT}/target/release/mimiclaw"
    )
    
    for path in "${possible_paths[@]}"; do
        if [[ -x "$path" ]]; then
            info "Found mimiclaw binary: $path"
            return 0
        fi
    done
    
    error "Could not find mimiclaw binary. Tried:"
    for path in "${possible_paths[@]}"; do
        error "  - $path"
    done
    error "Please build mimiclaw first before running tests."
    return 1
}

create_venv() {
    if [[ -d "$VENV_DIR" ]]; then
        if [[ "$CLEAN" == "true" ]]; then
            info "Cleaning existing virtual environment..."
            rm -rf "$VENV_DIR"
        else
            info "Using existing virtual environment"
            return 0
        fi
    fi
    
    info "Creating Python virtual environment..."
    if command -v python3 &> /dev/null; then
        python3 -m venv "$VENV_DIR"
    elif command -v python &> /dev/null; then
        python -m venv "$VENV_DIR"
    else
        error "Python not found. Please install Python 3.8+."
        exit 1
    fi
    
    # Upgrade pip
    source "${VENV_DIR}/bin/activate"
    pip install --upgrade pip
    
    info "Installing dependencies..."
    pip install -r "$REQUIREMENTS_FILE"
    
    info "Virtual environment created successfully!"
}

cleanup() {
    info "Cleaning up test processes..."
    pkill -f "virtual_llm_server\|mimiclaw\|test_mcp" 2>/dev/null || true
}

# Parse arguments
USE_VENV=true
CLEAN=false
PYTEST_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help)
            show_help
            exit 0
            ;;
        --no-venv)
            USE_VENV=false
            shift
            ;;
        --clean)
            CLEAN=true
            shift
            ;;
        *)
            PYTEST_ARGS+=("$1")
            shift
            ;;
    esac
done

# Main execution
info "========================================"
info "    MCP Integration Test Runner"
info "========================================"

# Check if mimiclaw binary exists
if ! check_mimiclaw_binary; then
    exit 1
fi

# Clean up any leftover processes
cleanup

# Set up virtual environment if needed
if [[ "$USE_VENV" == "true" ]]; then
    create_venv
    source "${VENV_DIR}/bin/activate"
else
    warn "Using system Python (no virtual environment)"
    # Check if dependencies are installed (with PYTHONPATH support)
    if ! python3 -c "import flask, requests, pytest" 2>/dev/null; then
        # Try with our custom Python path
        export PYTHONPATH="/tmp/pypkgs:${SCRIPT_DIR}"
        if ! python3 -c "import flask, requests, pytest" 2>/dev/null; then
            error "Dependencies not found. Please install them first:"
            error "  pip install -r ${REQUIREMENTS_FILE}"
            error "Or in this environment:"
            error "  pip install --break-system-packages -r ${REQUIREMENTS_FILE} --target=/tmp/pypkgs"
            exit 1
        fi
    fi
    # Ensure PYTHONPATH is set for test execution
    export PYTHONPATH="/tmp/pypkgs:${SCRIPT_DIR}:${PYTHONPATH:-}"
fi

# Run tests
info "Running integration tests..."
cd "$SCRIPT_DIR"

if [[ ${#PYTEST_ARGS[@]} -eq 0 ]]; then
    # Default test run
    python3 -m pytest test_mcp_integration.py -v --tb=short
else
    # Check if user specified a test file or class, otherwise prepend the test file
    if [[ " ${PYTEST_ARGS[*]} " =~ "test_mcp_integration.py" ]]; then
        # User already specified the test file
        info "Test arguments: ${PYTEST_ARGS[*]}"
        python3 -m pytest "${PYTEST_ARGS[@]}"
    else
        # Prepend the test file and pass all other arguments
        info "Test arguments: test_mcp_integration.py ${PYTEST_ARGS[*]}"
        python3 -m pytest test_mcp_integration.py "${PYTEST_ARGS[@]}"
    fi
fi

TEST_EXIT_CODE=$?

# Cleanup
cleanup

info "========================================"
if [[ $TEST_EXIT_CODE -eq 0 ]]; then
    info "✅ All tests passed!"
else
    error "❌ Some tests failed!"
fi
info "========================================"

exit $TEST_EXIT_CODE
