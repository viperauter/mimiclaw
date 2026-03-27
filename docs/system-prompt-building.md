# System Prompt Building Process

## Overview

The system prompt for MimiClaw is dynamically built from multiple sources to provide comprehensive context for the AI assistant. This document explains how the prompt is constructed and the components that contribute to it.

## Core Function

The system prompt is built by the `context_build_system_prompt` function located in `main/agent/context_builder.c`. This function assembles information from various files and sources into a structured Markdown format.

## Building Steps

### 1. Basic Information

The process starts with adding fundamental information about MimiClaw:

```c
off += snprintf(buf + off, size - off,
    "# MimiClaw\n\n"
    "You are MimiClaw, a personal AI assistant.\n"
    "Be helpful, accurate, and concise.\n");
```

### 2. Agent Guidelines

Next, the function attempts to add content from the agent guidelines file (AGENTS.md):

```c
off = append_file(buf, size, off, MIMI_DEFAULT_AGENTS_FILE, "Agent Guidelines");
```

If the file doesn't exist, this step is gracefully skipped.

### 3. Memory and Skills Information

Basic information about memory and skills is added:

```c
off += snprintf(buf + off, size - off,
    "\n## Memory\n\n"
    "- Long-term memory: %s\n"
    "- Daily notes: %s/daily/<YYYY-MM-DD>.md\n\n"
    "Use memory to retain durable user facts and preferences.\n\n"
    "## Skills\n\n"
    "Skills live under %s. When relevant, load the skill file before acting.\n",
    memory_file,
    "memory",
    skills_prefix);
```

### 4. Bootstrap Files

The function adds content from the personality file (SOUL.md) and user information file (USER.md):

```c
off = append_file(buf, size, off, soul_file, "Personality");
off = append_file(buf, size, off, user_file, "User Info");
```

### 5. Long-term Memory

Content from the long-term memory file (MEMORY.md) is added:

```c
char mem_buf[4096];
if (memory_read_long_term(mem_buf, sizeof(mem_buf)) == MIMI_OK && mem_buf[0]) {
    off += snprintf(buf + off, size - off, "\n## Long-term Memory\n\n%s\n", mem_buf);
}
```

### 6. Recent Daily Notes

The most recent 3 days of daily notes are added:

```c
char recent_buf[4096];
if (memory_read_recent(recent_buf, sizeof(recent_buf), 3) == MIMI_OK && recent_buf[0]) {
    off += snprintf(buf + off, size - off, "\n## Recent Notes\n\n%s\n", recent_buf);
}
```

### 7. Available Skills

A summary of available skills is built and added:

```c
char skills_buf[2048];
size_t skills_len = skill_loader_build_summary(skills_buf, sizeof(skills_buf));
if (skills_len > 0) {
    off += snprintf(buf + off, size - off,
        "\n## Available Skills\n\n"
        "Available skills (use read_file to load full instructions):\n%s\n",
        skills_buf);
}
```

## Key Files

The system prompt construction uses several key files:

| File Path | Purpose | Default Location |
|-----------|---------|------------------|
| SOUL.md | Contains AI personality information | Workspace root |
| USER.md | Contains user information | Workspace root |
| memory/MEMORY.md | Contains long-term memory | memory directory |
| memory/daily/YYYY-MM-DD.md | Contains daily notes | memory/daily directory |
| skills/ | Contains various skill files | skills directory |
| AGENTS.md | Contains agent guidelines | Workspace root |

## Helper Functions

### append_file

This function reads a file and appends its content to the prompt, optionally adding a header:

```c
static size_t append_file(char *buf, size_t size, size_t offset, const char *path, const char *header)
```

### memory_read_long_term

Reads content from the long-term memory file (MEMORY.md).

### memory_read_recent

Reads recent daily notes (default: last 3 days).

### skill_loader_build_summary

Builds a summary of all available skills, including their titles and descriptions.

## Configuration Management

The system uses configuration to manage file paths:
- Paths are read from the configuration
- Default paths are used if not specified in the configuration

## Resulting Structure

The final system prompt is a structured Markdown document with the following sections:

1. **Basic System Information** - Name and core instructions
2. **Agent Guidelines** (if present)
3. **Memory and Skills Overview** - Basic information about available resources
4. **Personality** - Content from SOUL.md
5. **User Info** - Content from USER.md
6. **Long-term Memory** - Content from MEMORY.md
7. **Recent Notes** - Content from recent daily notes
8. **Available Skills** - Summary of available skills

## Technical Features

1. **Modular Design** - Different information sources are processed separately
2. **Fault Tolerance** - Gracefully handles missing or empty files
3. **Structured Organization** - Uses Markdown format for clear structure
4. **Dynamic Content** - Includes real-time memory and skill information

This approach ensures the AI assistant has comprehensive context to provide personalized and accurate responses to user queries.
