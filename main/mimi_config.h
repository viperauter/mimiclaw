#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ============================================
// Memory Configuration
// ============================================

// Memory profile presets
#define MIMI_MEM_PROFILE_DEFAULT 0
#define MIMI_MEM_PROFILE_EMBEDDED 1
#define MIMI_MEM_PROFILE_HIGH_PERF 2

// Select memory profile (via CMake: -DMIMI_MEM_PROFILE=1)
#ifndef MIMI_MEM_PROFILE
#define MIMI_MEM_PROFILE MIMI_MEM_PROFILE_DEFAULT
#endif

#if MIMI_MEM_PROFILE == MIMI_MEM_PROFILE_EMBEDDED
// Embedded configuration (suitable for MCU, RAM < 512K)
#define MIMI_MAX_CONCURRENT 2
#define MIMI_TOOL_CALL_MAX_CONTEXTS 4
#define MIMI_CONTEXT_BUF_SIZE (8 * 1024)
#define MIMI_LLM_STREAM_BUF_SIZE (16 * 1024)
#define MIMI_TOOL_OUTPUT_SIZE (4 * 1024)
#define MIMI_CHANNEL_NAME_LEN 64
#define MIMI_CHAT_ID_LEN 128
#define MIMI_MAX_ITERS 10

#elif MIMI_MEM_PROFILE == MIMI_MEM_PROFILE_HIGH_PERF
// High performance configuration (suitable for servers, RAM > 2G)
#define MIMI_MAX_CONCURRENT 16
#define MIMI_TOOL_CALL_MAX_CONTEXTS 32
#define MIMI_CONTEXT_BUF_SIZE (64 * 1024)
#define MIMI_LLM_STREAM_BUF_SIZE (64 * 1024)
#define MIMI_TOOL_OUTPUT_SIZE (64 * 1024)
#define MIMI_CHANNEL_NAME_LEN 64
#define MIMI_CHAT_ID_LEN 128
#define MIMI_MAX_ITERS 20

#elif MIMI_MEM_PROFILE == MIMI_MEM_PROFILE_MIN_ASYNC
// Minimal async configuration (async mode but with minimal memory)
#define MIMI_MAX_CONCURRENT 1
#define MIMI_TOOL_CALL_MAX_CONTEXTS 1
#define MIMI_CONTEXT_BUF_SIZE (16 * 1024)
#define MIMI_LLM_STREAM_BUF_SIZE (32 * 1024)
#define MIMI_TOOL_OUTPUT_SIZE (32 * 1024)
#define MIMI_CHANNEL_NAME_LEN 64
#define MIMI_CHAT_ID_LEN 128
#define MIMI_MAX_ITERS 10

#else
// Default configuration (suitable for desktop/server, RAM > 1G)
#define MIMI_MAX_CONCURRENT 8
#define MIMI_TOOL_CALL_MAX_CONTEXTS 16
#define MIMI_CONTEXT_BUF_SIZE (16 * 1024)
#define MIMI_LLM_STREAM_BUF_SIZE (32 * 1024)
#define MIMI_TOOL_OUTPUT_SIZE (32 * 1024)
#define MIMI_CHANNEL_NAME_LEN 64
#define MIMI_CHAT_ID_LEN 128
#define MIMI_MAX_ITERS 10
#endif

// ============================================
// Feature Configuration
// ============================================

// Enable/disable channels (via CMake: -DMIMI_ENABLE_FEISHU=0)
#ifndef MIMI_ENABLE_CLI
#define MIMI_ENABLE_CLI 1
#endif

#ifndef MIMI_ENABLE_FEISHU
#define MIMI_ENABLE_FEISHU 1
#endif

#ifndef MIMI_ENABLE_TELEGRAM
#define MIMI_ENABLE_TELEGRAM 1
#endif

#ifndef MIMI_ENABLE_QQ
#define MIMI_ENABLE_QQ 1
#endif

#ifndef MIMI_ENABLE_WS_SERVICE
#define MIMI_ENABLE_WS_SERVICE 1
#endif

// Enable/disable gateways
#ifndef MIMI_ENABLE_STDIO_GATEWAY
#define MIMI_ENABLE_STDIO_GATEWAY 1
#endif

#ifndef MIMI_ENABLE_WS_SERVER_GATEWAY
#define MIMI_ENABLE_WS_SERVER_GATEWAY 1
#endif

#ifndef MIMI_ENABLE_WS_CLIENT_GATEWAY
#define MIMI_ENABLE_WS_CLIENT_GATEWAY 1
#endif

#ifndef MIMI_ENABLE_HTTP_CLIENT_GATEWAY
#define MIMI_ENABLE_HTTP_CLIENT_GATEWAY 1
#endif

// Enable/disable services
#ifndef MIMI_ENABLE_CRON_SERVICE
#define MIMI_ENABLE_CRON_SERVICE 1
#endif

#ifndef MIMI_ENABLE_HEARTBEAT_SERVICE
#define MIMI_ENABLE_HEARTBEAT_SERVICE 1
#endif

// Enable/disable tools
#ifndef MIMI_ENABLE_TOOL_FILES
#define MIMI_ENABLE_TOOL_FILES 1
#endif

#ifndef MIMI_ENABLE_TOOL_WEB_SEARCH
#define MIMI_ENABLE_TOOL_WEB_SEARCH 1
#endif

#ifndef MIMI_ENABLE_TOOL_GET_TIME
#define MIMI_ENABLE_TOOL_GET_TIME 1
#endif

#ifndef MIMI_ENABLE_TOOL_CRON
#define MIMI_ENABLE_TOOL_CRON 1
#endif

// Enable/disable skill system
#ifndef MIMI_ENABLE_SKILL_SYSTEM
#define MIMI_ENABLE_SKILL_SYSTEM 1
#endif

// ============================================
// Logging Configuration
// ============================================

// Log levels (via CMake: -DMIMI_LOG_LEVEL=2)
#define MIMI_LOG_LEVEL_NONE 0
#define MIMI_LOG_LEVEL_ERROR 1
#define MIMI_LOG_LEVEL_WARN 2
#define MIMI_LOG_LEVEL_INFO 3
#define MIMI_LOG_LEVEL_DEBUG 4
#define MIMI_LOG_LEVEL_VERBOSE 5

#ifndef MIMI_LOG_LEVEL
#define MIMI_LOG_LEVEL MIMI_LOG_LEVEL_INFO
#endif

// Enable/disable logging for specific modules
#ifndef MIMI_LOG_ENABLE_AGENT
#define MIMI_LOG_ENABLE_AGENT 1
#endif

#ifndef MIMI_LOG_ENABLE_LLM
#define MIMI_LOG_ENABLE_LLM 1
#endif

#ifndef MIMI_LOG_ENABLE_TOOLS
#define MIMI_LOG_ENABLE_TOOLS 1
#endif

#ifndef MIMI_LOG_ENABLE_CHANNELS
#define MIMI_LOG_ENABLE_CHANNELS 1
#endif

#ifndef MIMI_LOG_ENABLE_GATEWAYS
#define MIMI_LOG_ENABLE_GATEWAYS 1
#endif

// ============================================
// Security Configuration
// ============================================

// Enable/disable tool confirmation mechanism
#ifndef MIMI_ENABLE_TOOL_CONFIRMATION
#define MIMI_ENABLE_TOOL_CONFIRMATION 1
#endif

// Tool confirmation timeout in seconds
#ifndef MIMI_TOOL_CONFIRMATION_TIMEOUT
#define MIMI_TOOL_CONFIRMATION_TIMEOUT 300
#endif

// ============================================
// Performance Configuration
// ============================================

// LLM request timeout in seconds
#ifndef MIMI_LLM_REQUEST_TIMEOUT
#define MIMI_LLM_REQUEST_TIMEOUT 120
#endif

// HTTP request timeout in seconds
#ifndef MIMI_HTTP_REQUEST_TIMEOUT
#define MIMI_HTTP_REQUEST_TIMEOUT 30
#endif

// WebSocket heartbeat interval in seconds
#ifndef MIMI_WS_HEARTBEAT_INTERVAL
#define MIMI_WS_HEARTBEAT_INTERVAL 30
#endif

// ============================================
// Storage Configuration
// ============================================

// Maximum session history entries
#ifndef MIMI_MAX_SESSION_HISTORY
#define MIMI_MAX_SESSION_HISTORY 100
#endif

// Maximum number of workspaces
#ifndef MIMI_MAX_WORKSPACES
#define MIMI_MAX_WORKSPACES 16
#endif

// ============================================
// Backward Compatibility Macros
// ============================================

// Keep old macro definitions for backward compatibility
#define MAX_CONCURRENT MIMI_MAX_CONCURRENT
#define TOOL_CALL_MAX_CONTEXTS MIMI_TOOL_CALL_MAX_CONTEXTS
#define CONTEXT_BUF_SIZE MIMI_CONTEXT_BUF_SIZE
#define LLM_STREAM_BUF_SIZE MIMI_LLM_STREAM_BUF_SIZE
#define TOOL_OUTPUT_SIZE MIMI_TOOL_OUTPUT_SIZE
#define CHANNEL_NAME_LEN MIMI_CHANNEL_NAME_LEN
#define CHAT_ID_LEN MIMI_CHAT_ID_LEN

#ifdef __cplusplus
}
#endif
