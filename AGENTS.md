# MimiClaw Skills Documentation

## Project Overview

MimiClaw is a cross-platform AI Agent framework implemented in C, supporting POSIX systems (Linux/macOS) and embedded platforms. It transforms embedded devices into personal AI assistants with interactive capabilities.

### Key Features

- **Cross-platform Support**: Linux/macOS and generic embedded platforms
- **Multi-interface Communication**: Telegram, WebSocket, CLI, Feishu, WeChat, and other access methods
- **Event-driven Architecture**: I/O and business logic separation for efficient asynchronous processing
- **Low-power Design**: Optimized for embedded devices with minimal power consumption
- **Local Memory**: Data stored in local flash memory with persistent storage
- **Multi-model Support**: Anthropic (Claude) and OpenAI (GPT) integration
- **MCP Integration**: Model Context Protocol support for external tool integration
- **Subagent Orchestration**: In-process subagent management for task decomposition
- **Context Management**: Smart context budget planning, assembly, and LLM-based compaction
- **Tool Call Control**: User confirmation mechanism and general control channel for tool execution
- **System Prompt Building**: Dynamic system prompt construction from multiple sources
- **WeChat Integration**: Native WeChat iLink Bot API support for WeChat messaging

### Technical Architecture

MimiClaw adopts a layered architecture design:

```
┌─────────────────────────────────────────────────────┐
│         External Interfaces (Telegram/WS/CLI/Feishu/WeChat) │
└──────────────────┬──────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────┐
│         Gateway Layer (Transport Abstraction)        │
└──────────────────┬──────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────┐
│         Channel Layer (Business Logic)               │
└──────────────────┬──────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────┐
│         Message Bus Layer (Unified Communication)    │
└──────────────────┬──────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────┐
│         Core System (Agent/LLM/Tools/Storage/Context) │
└─────────────────────────────────────────────────────┘
```

### Technology Stack

- **Hardware**: Generic embedded devices or x86/arm64 PC
- **Framework**: POSIX (desktop) / generic embedded platforms
- **Language**: C
- **Network**: Mongoose (HTTP/WebSocket)
- **Storage**: VFS (POSIX)

---

## Document Directory

| Section | Description |
|---------|-------------|
| [Quick Start](docs/quickstart.md) | Environment setup, compilation, execution |
| [MCP Integration](docs/mcp.md) | Model Context Protocol implementation |
| [Subagent Orchestration](docs/subagent.md) | In-process subagent management |
| [Context Management](docs/context.md) | Context budget planning, assembly, and LLM-based compaction |
| [System Prompt Building](docs/system-prompt-building.md) | Dynamic system prompt construction from memory, skills, and configuration |
| [Tool Call Control](docs/tool_call.md) | User confirmation mechanism and general control channel for tool execution |
| [WeChat Integration](docs/wechat.md) | WeChat iLink Bot API protocol and implementation |
| [Architecture Design](docs/ARCHITECTURE.md) | System architecture, module explanations |
| [Deployment Guide](skills/deploy/SKILL.md) | Program packaging, device upgrade, environment configuration, automated deployment |
| [Debugging Guide](docs/debug.md) | Log redirection, debugging techniques |

---

*Last updated: 2026-03-27*
