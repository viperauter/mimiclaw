#!/bin/bash

# Build script for mimiclaw project

# Colors for output
GREEN="\033[0;32m"
YELLOW="\033[1;33m"
RED="\033[0;31m"
NC="\033[0m" # No Color

# Default values
BUILD_DIR="./build"
FULL_BUILD=false
SILENT_MODE=false

# Usage information
usage() {
    echo -e "${YELLOW}Usage: $0 [options]${NC}"
    echo -e "Options:"
    echo -e "  -f, --full    Perform full build (clean and rebuild)"
    echo -e "  -s, --silent  Silent mode (only show errors and warnings)"
    echo -e "  -h, --help    Show this help message"
    exit 1
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--full)
            FULL_BUILD=true
            shift
            ;;
        -s|--silent)
            SILENT_MODE=true
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            usage
            ;;
    esac
    shift
done

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}Build directory not found, creating...${NC}"
    mkdir -p "$BUILD_DIR"
    
    # Run CMake configuration
    echo -e "${GREEN}Running CMake configuration...${NC}"
    cd "$BUILD_DIR" && cmake ..
    if [ $? -ne 0 ]; then
        echo -e "${RED}CMake configuration failed!${NC}"
        exit 1
    fi
    cd ..
fi

# Handle full build
if [ "$FULL_BUILD" = true ]; then
    echo -e "${YELLOW}Performing full build...${NC}"
    
    # Clean build directory
    echo -e "${GREEN}Cleaning build directory...${NC}"
    rm -rf "$BUILD_DIR"/*
    
    # Reconfigure CMake
    echo -e "${GREEN}Reconfiguring CMake...${NC}"
    cd "$BUILD_DIR" && cmake ..
    if [ $? -ne 0 ]; then
        echo -e "${RED}CMake configuration failed!${NC}"
        exit 1
    fi
    cd ..
else
    echo -e "${GREEN}Performing incremental build...${NC}"
fi

# Build the project
echo -e "${GREEN}Building project...${NC}"

if [ "$SILENT_MODE" = true ]; then
    echo -e "${YELLOW}Silent mode: only errors and warnings will be shown${NC}"
    # Run make and capture output, only show errors and warnings
    cd "$BUILD_DIR" && make 2>&1 | grep -E 'error|warning|failed|error:|warning:'
    MAKE_EXIT_CODE=$?
else
    # Run make normally with full output
    cd "$BUILD_DIR" && make
    MAKE_EXIT_CODE=$?
fi

if [ $MAKE_EXIT_CODE -eq 0 ]; then
    echo -e "${GREEN}Build completed successfully!${NC}"
else
    echo -e "${RED}Build failed!${NC}"
    exit 1
fi

cd ..
echo -e "${GREEN}Build script finished.${NC}"
