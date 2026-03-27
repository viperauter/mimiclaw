# Quick Start

## Prerequisites

- POSIX-compliant system (Linux/macOS)
- C compiler (GCC or Clang)
- CMake
- Git

## Compilation

### Basic Compilation

Run the build script from the project root directory:

```bash
./build.sh
```

This will compile the project and output artifacts in the `build/` directory.

### Full Rebuild

To perform a clean full rebuild:

```bash
./build.sh -f
```

The `-f` flag forces a complete rebuild from scratch, useful when you want to ensure all files are recompiled.

## Running the Application

After successful compilation, the executable will be located at `build/mimiclaw`.

```bash
./build/mimiclaw
```

---

*Last updated: 2026-03-27*
