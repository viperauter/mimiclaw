# 工具调用用户确认与通用控制通道实现方案

## 1. 概述

本方案旨在为Mimiclaw框架添加通用控制通道机制，不仅支持工具调用的用户确认，还支持停止操作、状态查询等多种控制场景。通过消息总线和异步状态管理，实现安全、流畅的用户交互流程。

## 2. 核心设计

### 2.1 工具结构扩展

在`tool_registry.h`中的`mimi_tool_t`结构体添加确认相关字段：

```c
typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;  /* JSON Schema string for input */
    bool requires_confirmation;     /* Whether this tool requires user confirmation */
    mimi_err_t (*execute)(const char *input_json, char *output, size_t output_size,
                          const mimi_session_ctx_t *session_ctx);
} mimi_tool_t;
```

### 2.2 工具调用上下文管理器

工具调用上下文管理器负责管理工具调用的生命周期和状态。创建新文件`tool_call_context.h`和`tool_call_context.c`：

**tool_call_context.h:**
```c
#ifndef TOOL_CALL_CONTEXT_H
#define TOOL_CALL_CONTEXT_H

#include "mimi_err.h"
#include "memory/session_mgr.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of concurrent tool call contexts */
#define TOOL_CALL_MAX_CONTEXTS 16

/* Tool output buffer size */
#define TOOL_OUTPUT_SIZE 32768

/* 确认结果枚举 */
typedef enum {
    CONFIRMATION_PENDING = 0,        /* 等待确认 */
    CONFIRMATION_ACCEPTED,           /* 用户确认执行 */
    CONFIRMATION_ACCEPTED_ALWAYS,    /* 用户确认并设置为总是允许 */
    CONFIRMATION_REJECTED,           /* 用户拒绝执行 */
    CONFIRMATION_TIMEOUT             /* 确认超时 */
} confirmation_result_t;

/* 工具调用上下文结构体 */
typedef struct {
    /* 工具基本信息 */
    char tool_name[64];
    char tool_call_id[128];
    char tool_input[1024];

    /* 执行上下文 */
    void *agent_ctx;  /* Opaque pointer to agent request context */
    mimi_session_ctx_t session_ctx;

    /* 确认状态 */
    bool requires_confirmation;
    bool waiting_for_confirmation;
    uint64_t confirmation_timeout;
    confirmation_result_t confirmation_result;

    /* 执行结果 */
    bool executed;
    bool succeeded;
    char output[TOOL_OUTPUT_SIZE];
    mimi_err_t error_code;

    /* 异步管理 */
    void *async_ctx;
    void *user_data;

    /* Internal state */
    bool active;
    uint32_t ref_count;
} tool_call_context_t;

/**
 * Initialize the tool call context manager
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t tool_call_context_manager_init(void);

/**
 * Deinitialize the tool call context manager
 */
void tool_call_context_manager_deinit(void);

/**
 * Create a new tool call context
 *
 * @param agent_ctx Agent request context
 * @param tool_name Tool name
 * @param tool_call_id Tool call ID from LLM
 * @param tool_input Tool input JSON
 * @param requires_confirmation Whether this tool requires user confirmation
 * @return Pointer to created context, NULL on failure
 */
tool_call_context_t *tool_call_context_create(
    void *agent_ctx,
    const char *tool_name,
    const char *tool_call_id,
    const char *tool_input,
    bool requires_confirmation
);

/**
 * Destroy a tool call context
 * Actually decreases reference count, only frees when count reaches 0
 *
 * @param tool_ctx Tool call context to destroy
 */
void tool_call_context_destroy(tool_call_context_t *tool_ctx);

/**
 * Retain a tool call context (increase reference count)
 *
 * @param tool_ctx Tool call context
 */
void tool_call_context_retain(tool_call_context_t *tool_ctx);

/**
 * Find a pending tool context by channel and chat_id
 *
 * @param channel Channel name
 * @param chat_id Chat ID
 * @return Pointer to pending context, NULL if not found
 */
tool_call_context_t *tool_call_context_find_pending(
    const char *channel,
    const char *chat_id
);

/**
 * Find a tool context by tool call ID
 *
 * @param tool_call_id Tool call ID
 * @return Pointer to context, NULL if not found
 */
tool_call_context_t *tool_call_context_find_by_id(const char *tool_call_id);

/**
 * Check for confirmation timeouts
 * Should be called periodically
 */
void tool_call_context_check_timeouts(void);

/**
 * Get the number of pending tool contexts
 *
 * @return Number of pending contexts
 */
int tool_call_context_get_pending_count(void);

/**
 * Get the number of active tool contexts
 *
 * @return Number of active contexts
 */
int tool_call_context_get_active_count(void);

/**
 * Mark a tool as always allowed
 *
 * @param tool_name Tool name to mark
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t tool_call_context_mark_always_allowed(const char *tool_name);

/**
 * Check if a tool is always allowed
 *
 * @param tool_name Tool name to check
 * @return true if always allowed, false otherwise
 */
bool tool_call_context_is_always_allowed(const char *tool_name);

/**
 * Load always allowed tools from persistent storage
 * Should be called during initialization
 *
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t tool_call_context_load_always_allowed(void);

/**
 * Save always allowed tools to persistent storage
 *
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t tool_call_context_save_always_allowed(void);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_CALL_CONTEXT_H */
```

### 2.3 确认状态管理

在`agent_async_loop.c`中扩展`agent_request_ctx_t`结构体，添加确认相关状态：

```c
typedef struct {
    char channel[64];
    char chat_id[128];
    char content[32768];
    char *system_prompt;
    cJSON *messages;
    const char *tools_json;
    int iteration;
    int max_iters;
    bool sent_working_status;
    char tool_output[TOOL_OUTPUT_SIZE];
    llm_response_t llm_resp;
    bool in_progress;
    
    /* 工具调用上下文管理 */
    tool_call_context_t *pending_tool_ctx;
    int pending_tool_count;
} agent_request_ctx_t;
```

### 2.3 异步确认流程

1. **检查工具是否需要确认**：在`start_async_tools`函数中，对每个工具调用进行确认检查
2. **发送确认请求**：如果工具需要确认，向channel发送确认请求
3. **等待用户响应**：进入等待状态，通过消息总线接收用户的确认响应
4. **处理确认结果**：根据用户的响应执行或取消工具操作
5. **超时处理**：设置合理的超时时间，超时后自动取消操作

## 3. 详细实现

### 3.1 工具注册修改

在`tool_registry.c`中注册工具时，为敏感工具设置确认标志：

| 工具名称 | 需要确认 | 原因 |
|---------|---------|------|
| web_search | false | 无风险操作 |
| get_current_time | false | 无风险操作 |
| read_file | false | 只读操作 |
| write_file | true | 可能覆盖文件 |
| edit_file | true | 可能修改文件内容 |
| list_dir | false | 无风险操作 |
| cron_add | true | 可能设置长期任务 |
| cron_list | false | 无风险操作 |
| cron_remove | false | 移除任务，风险较低 |

### 3.2 工具调用上下文管理

实现工具调用上下文的创建和管理函数：

```c
static tool_call_context_t *create_tool_call_context(agent_request_ctx_t *agent_ctx, 
                                                  const llm_tool_call_t *call,
                                                  const char *tool_input,
                                                  bool requires_confirmation)
{
    tool_call_context_t *tool_ctx = (tool_call_context_t *)calloc(1, sizeof(tool_call_context_t));
    if (!tool_ctx) {
        return NULL;
    }
    
    // 设置工具基本信息
    strncpy(tool_ctx->tool_name, call->name, sizeof(tool_ctx->tool_name) - 1);
    strncpy(tool_ctx->tool_call_id, call->id, sizeof(tool_ctx->tool_call_id) - 1);
    strncpy(tool_ctx->tool_input, tool_input, sizeof(tool_ctx->tool_input) - 1);
    
    // 设置执行上下文
    tool_ctx->agent_ctx = agent_ctx;
    
    // 创建会话上下文
    mimi_msg_t temp_msg = {0};
    strncpy(temp_msg.channel, agent_ctx->channel, sizeof(temp_msg.channel) - 1);
    strncpy(temp_msg.chat_id, agent_ctx->chat_id, sizeof(temp_msg.chat_id) - 1);
    session_ctx_from_msg(&temp_msg, &tool_ctx->session_ctx);
    
    // 设置确认状态
    tool_ctx->requires_confirmation = requires_confirmation;
    if (requires_confirmation) {
        tool_ctx->waiting_for_confirmation = true;
        tool_ctx->confirmation_timeout = mimi_time_ms() + 30000; // 30秒超时
        tool_ctx->confirmation_result = CONFIRMATION_PENDING;
    }
    
    return tool_ctx;
}

static void destroy_tool_call_context(tool_call_context_t *tool_ctx)
{
    if (!tool_ctx) {
        return;
    }
    
    // 清理异步上下文
    if (tool_ctx->async_ctx) {
        if (tool_ctx->async_ctx->mutex) {
            mimi_mutex_destroy(tool_ctx->async_ctx->mutex);
        }
        free(tool_ctx->async_ctx);
    }
    
    // 清理用户数据
    if (tool_ctx->user_data) {
        if (tool_ctx->user_data->tool_call_id) {
            free(tool_ctx->user_data->tool_call_id);
        }
        free(tool_ctx->user_data);
    }
    
    free(tool_ctx);
}
```

### 3.3 确认请求机制

实现`send_confirmation_request`函数，使用控制管理器：

```c
static void confirm_callback(const char *request_id, const char *response, void *context)
{
    tool_call_context_t *tool_ctx = (tool_call_context_t *)context;
    if (!tool_ctx || !tool_ctx->agent_ctx) {
        return;
    }
    
    // 解析确认结果
    if (strcmp(response, "ACCEPT") == 0) {
        tool_ctx->confirmation_result = CONFIRMATION_ACCEPTED;
        MIMI_LOGI(TAG, "Tool execution confirmed: %s", tool_ctx->tool_name);
        execute_tool_async(tool_ctx);
    } else if (strcmp(response, "ACCEPT_ALWAYS") == 0) {
        tool_ctx->confirmation_result = CONFIRMATION_ACCEPTED_ALWAYS;
        MIMI_LOGI(TAG, "Tool execution confirmed (always): %s", tool_ctx->tool_name);
        mark_tool_always_allowed(tool_ctx->tool_name);
        execute_tool_async(tool_ctx);
    } else {
        tool_ctx->confirmation_result = CONFIRMATION_REJECTED;
        MIMI_LOGI(TAG, "Tool execution rejected: %s", tool_ctx->tool_name);
        cancel_pending_tool(tool_ctx);
    }
    
    tool_ctx->waiting_for_confirmation = false;
}

static void send_confirmation_request(tool_call_context_t *tool_ctx)
{
    if (!tool_ctx || !tool_ctx->agent_ctx) {
        return;
    }

    agent_request_ctx_t *ctx = tool_ctx->agent_ctx;
    
    // 构建确认请求数据
    char data[1024];
    snprintf(data, sizeof(data), "参数：%s", tool_ctx->tool_input);
    
    // 使用控制管理器发送确认请求
    char request_id[64];
    mimi_err_t err = control_manager_send_request(
        ctx->channel,
        ctx->chat_id,
        MIMI_CONTROL_TYPE_CONFIRM,
        tool_ctx->tool_name,
        data,
        tool_ctx,
        confirm_callback,
        request_id
    );
    
    if (err == MIMI_OK) {
        MIMI_LOGI(TAG, "Sent confirmation request for tool: %s, request_id: %s", 
                 tool_ctx->tool_name, request_id);
    } else {
        MIMI_LOGE(TAG, "Failed to send confirmation request: %s", mimi_err_to_name(err));
    }
}
```

### 3.5 控制消息处理机制

在`agent_async_loop_task`中添加控制消息处理：

```c
while (s_agent_running && !mimi_runtime_should_exit()) {
    mimi_msg_t msg;
    mimi_err_t err = message_bus_pop_inbound(&msg, 100);
    if (err != MIMI_OK) {
        // 检查控制请求超时
        control_manager_check_timeouts();
        continue;
    }
    
    // 处理控制消息
    if (msg.type == MIMI_MSG_TYPE_CONTROL) {
        // 交给控制管理器处理
        control_manager_handle_response(msg.request_id, msg.content ? msg.content : "");
        free(msg.content);
        continue;
    }
    
    // 原有消息处理逻辑...
}
```

### 3.6 控制管理器回调处理

控制管理器会根据不同的控制类型调用相应的回调函数。对于确认请求，我们已经在`confirm_callback`函数中处理了用户响应。

对于其他控制类型，如停止操作，可以添加相应的回调处理：

```c
static void stop_callback(const char *request_id, const char *response, void *context)
{
    // 处理停止操作的响应
    if (strcmp(response, "STOPPED") == 0) {
        MIMI_LOGI(TAG, "Operation stopped successfully");
    } else {
        MIMI_LOGW(TAG, "Operation not running");
    }
}

// 发送停止请求的示例
void send_stop_request(const char *channel, const char *chat_id, const char *operation_id)
{
    char request_id[64];
    mimi_err_t err = control_manager_send_request(
        channel,
        chat_id,
        MIMI_CONTROL_TYPE_STOP,
        operation_id,
        "",
        NULL,
        stop_callback,
        request_id
    );
}
```

### 3.4 工具执行修改

修改`start_async_tools`函数，使用工具调用上下文：

```c
static void start_async_tools(agent_request_ctx_t *ctx, const llm_response_t *resp) {
    if (!resp) {
        MIMI_LOGE(TAG, "Null response pointer in start_async_tools");
        return;
    }
    
    tool_async_ctx_t *async_ctx = (tool_async_ctx_t *)malloc(sizeof(tool_async_ctx_t));
    if (!async_ctx) {
        MIMI_LOGE(TAG, "Failed to allocate async tool context");
        return;
    }
    
    async_ctx->ctx = ctx;
    async_ctx->total_calls = resp->call_count;
    async_ctx->completed_calls = 0;
    async_ctx->mutex = NULL;
    async_ctx->next_llm_started = false;
    
    mimi_err_t err = mimi_mutex_create(&async_ctx->mutex);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to create mutex: %s", mimi_err_to_name(err));
        free(async_ctx);
        return;
    }
    
    // 处理每个工具调用
    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        const char *tool_input = call->input ? call->input : "{}";
        char *patched_input = patch_tool_input_with_context(call, ctx->channel, ctx->chat_id);
        bool requires_confirmation = tool_requires_confirmation(call->name);
        
        // 创建工具调用上下文
        tool_call_context_t *tool_ctx = create_tool_call_context(ctx, call, 
                                                             patched_input ? patched_input : tool_input,
                                                             requires_confirmation);
        if (!tool_ctx) {
            MIMI_LOGE(TAG, "Failed to create tool call context");
            free(patched_input);
            continue;
        }
        
        tool_ctx->async_ctx = async_ctx;
        
        // 检查工具是否需要确认
        if (requires_confirmation) {
            // 需要确认，发送确认请求
            MIMI_LOGI(TAG, "Tool %s requires confirmation", call->name);
            send_confirmation_request(tool_ctx);
            ctx->pending_tool_ctx = tool_ctx;
            free(patched_input);
            return; // 等待确认，暂停处理
        } else {
            // 不需要确认，直接执行
            MIMI_LOGI(TAG, "Async tool execution: %s", call->name);
            execute_tool_async(tool_ctx);
            free(patched_input);
        }
    }
}

static void execute_tool_async(tool_call_context_t *tool_ctx)
{
    if (!tool_ctx || !tool_ctx->async_ctx) {
        return;
    }
    
    tool_call_ud_t *ud = (tool_call_ud_t *)calloc(1, sizeof(*ud));
    if (!ud) {
        MIMI_LOGE(TAG, "Failed to allocate tool call user data");
        return;
    }
    
    ud->parent = tool_ctx->async_ctx;
    ud->tool_call_id = strdup(tool_ctx->tool_call_id);
    if (!ud->tool_call_id) {
        free(ud);
        return;
    }
    
    tool_ctx->user_data = ud;
    tool_ctx->executed = true;
    
    tool_registry_execute_async(tool_ctx->tool_name, tool_ctx->tool_input,
                             &tool_ctx->session_ctx, tool_async_callback, ud);
}
```

## 4. Channel交互

### 4.1 消息类型定义

在`message_bus.h`中添加控制消息类型：

```c
typedef enum {
    MIMI_MSG_TYPE_TEXT,           // 普通文本消息
    MIMI_MSG_TYPE_CONTROL,         // 控制消息（通用）
    MIMI_MSG_TYPE_TOOL_RESULT,     // 工具执行结果
} mimi_msg_type_t;

typedef enum {
    MIMI_CONTROL_TYPE_CONFIRM,     // 确认请求
    MIMI_CONTROL_TYPE_CANCEL,      // 取消操作
    MIMI_CONTROL_TYPE_STOP,        // 停止操作
    MIMI_CONTROL_TYPE_STATUS,      // 状态查询
} mimi_control_type_t;

typedef struct {
    char channel[16];
    char chat_id[128];
    char *content;
    mimi_msg_type_t type;
    
    // 控制消息专用字段
    mimi_control_type_t control_type;
    char request_id[64];            // 唯一请求ID
    char target[64];                // 目标（如工具名称、操作ID）
    char data[1024];                // 附加数据（如工具参数）
} mimi_msg_t;
```

### 4.2 Channel责任

- **接收控制消息**：Channel负责接收`MIMI_MSG_TYPE_CONTROL`类型的消息
- **展示控制界面**：Channel根据控制类型展示相应界面（如确认按钮、停止按钮等）
- **收集用户响应**：Channel收集用户的操作响应
- **发送响应结果**：Channel将用户的响应以`MIMI_MSG_TYPE_CONTROL`类型的消息发送回系统

### 4.3 响应格式

控制响应消息内容格式：
- **确认请求响应**：`ACCEPT`（确认执行）、`ACCEPT_ALWAYS`（总是允许）、`REJECT`（拒绝执行）
- **停止请求响应**：`STOPPED`（已停止）、`NOT_RUNNING`（未运行）
- **状态查询响应**：JSON格式的状态信息

## 5. 控制管理器

### 5.1 控制管理器设计

控制管理器是通用控制通道的核心组件，负责管理控制请求的生命周期和状态。创建新文件`control_manager.h`和`control_manager.c`：

**control_manager.h:**
```c
/**
 * Control Manager
 * 
 * The control manager is the core component of the generic control channel,
 * responsible for managing the lifecycle and state of control requests.
 * It supports various control scenarios including tool confirmation,
 * operation cancellation, stop operations, and status queries.
 */

#ifndef CONTROL_MANAGER_H
#define CONTROL_MANAGER_H

#include "mimi_err.h"
#include "bus/message_bus.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of concurrent control requests */
#define CONTROL_MAX_REQUESTS 16

/* Default timeout for control requests (30 seconds) */
#define CONTROL_DEFAULT_TIMEOUT_MS 30000

/* Request ID length */
#define CONTROL_REQUEST_ID_LEN 64

/* 控制请求结构体 */
typedef struct {
    char request_id[CONTROL_REQUEST_ID_LEN];
    char channel[16];
    char chat_id[128];
    mimi_control_type_t control_type;
    char target[64];
    void *context;                  /* Context pointer (e.g., tool_call_context_t) */
    uint64_t timeout;
    void (*callback)(const char *request_id, const char *response, void *context);
    bool active;
} control_request_t;

/**
 * Initialize the control manager
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t control_manager_init(void);

/**
 * Deinitialize the control manager
 */
void control_manager_deinit(void);

/**
 * Send a control request
 * 
 * @param channel Channel name (e.g., "telegram", "cli")
 * @param chat_id Chat ID or session ID
 * @param control_type Type of control request
 * @param target Target of the control (e.g., tool name)
 * @param data Additional data for the control request
 * @param context Context pointer to be passed to callback
 * @param callback Callback function to handle the response
 * @param out_request_id Output buffer for the generated request ID
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t control_manager_send_request(
    const char *channel,
    const char *chat_id,
    mimi_control_type_t control_type,
    const char *target,
    const char *data,
    void *context,
    void (*callback)(const char *request_id, const char *response, void *context),
    char *out_request_id
);

/**
 * Handle a control response
 * 
 * @param request_id Request ID from the control message
 * @param response Response string from the user
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t control_manager_handle_response(const char *request_id, const char *response);

/**
 * Handle a control response by chat ID
 * Finds the most recent pending request for the given chat ID
 * 
 * @param chat_id Chat ID or session ID
 * @param response Response string from the user
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t control_manager_handle_response_by_chat_id(const char *chat_id, const char *response);

/**
 * Check for timed out control requests
 * Should be called periodically (e.g., in the main loop)
 */
void control_manager_check_timeouts(void);

/**
 * Cancel a pending control request
 * 
 * @param request_id Request ID to cancel
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t control_manager_cancel_request(const char *request_id);

/**
 * Get the number of active control requests
 * 
 * @return Number of active requests
 */
int control_manager_get_active_count(void);

/**
 * Generate a unique request ID
 * 
 * @param out_id Output buffer for the request ID
 * @param id_len Length of the output buffer
 */
void control_manager_generate_request_id(char *out_id, size_t id_len);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_MANAGER_H */
```

### 5.2 控制流程

**发送控制请求：**
```
Agent 或其他组件
    ↓
control_manager_send_request()
    ↓
生成 request_id，存储上下文和回调
    ↓
构建控制消息 (mimi_msg_t with type=CONTROL)
    ↓
message_bus_push_outbound()
    ↓
Outbound Dispatch Task
    ↓
Channel 渲染控制界面（如确认按钮、停止按钮）
    ↓
用户操作
```

**处理控制响应：**
```
用户操作
    ↓
Channel 构建控制响应消息
    ↓
message_bus_push_inbound()
    ↓
Agent Async Loop
    ↓
检测到 CONTROL 类型消息
    ↓
control_manager_handle_response()
    ↓
查找对应的请求和回调
    ↓
调用回调函数处理响应
```

### 5.3 等待确认状态

- **状态标识**：`tool_call_context_t.waiting_for_confirmation`为true时，表示正在等待用户确认
- **超时处理**：定期检查确认超时，超时后自动取消操作
- **上下文保持**：保持工具调用的上下文信息，等待用户响应后恢复执行

### 5.2 总是允许状态

- **持久化存储**：将用户标记为总是允许的工具存储在持久化存储中
- **加载机制**：系统启动时加载总是允许的工具列表
- **检查机制**：执行工具前检查是否在总是允许列表中

### 5.3 工具上下文查找

实现查找待确认工具上下文的函数：

```c
static tool_call_context_t *find_pending_tool_ctx(const char *channel, const char *chat_id)
{
    if (!channel || !chat_id) {
        return NULL;
    }
    
    if (s_ctx_mutex) {
        mimi_mutex_lock(s_ctx_mutex);
    }
    
    tool_call_context_t *found_ctx = NULL;
    for (int i = 0; i < MAX_CONCURRENT; i++) {
        agent_request_ctx_t *ctx = &s_pending_ctx[i];
        if (ctx->in_progress && ctx->pending_tool_ctx) {
            tool_call_context_t *tool_ctx = ctx->pending_tool_ctx;
            if (strcmp(tool_ctx->agent_ctx->channel, channel) == 0 &&
                strcmp(tool_ctx->agent_ctx->chat_id, chat_id) == 0 &&
                tool_ctx->waiting_for_confirmation) {
                found_ctx = tool_ctx;
                break;
            }
        }
    }
    
    if (s_ctx_mutex) {
        mimi_mutex_unlock(s_ctx_mutex);
    }
    
    return found_ctx;
}
```

### 5.4 确认超时检查

实现确认超时检查函数：

```c
static void check_confirmation_timeouts(void)
{
    uint64_t current_time = mimi_time_ms();
    
    if (s_ctx_mutex) {
        mimi_mutex_lock(s_ctx_mutex);
    }
    
    for (int i = 0; i < MAX_CONCURRENT; i++) {
        agent_request_ctx_t *ctx = &s_pending_ctx[i];
        if (ctx->in_progress && ctx->pending_tool_ctx) {
            tool_call_context_t *tool_ctx = ctx->pending_tool_ctx;
            if (tool_ctx->waiting_for_confirmation && 
                current_time >= tool_ctx->confirmation_timeout) {
                
                MIMI_LOGW(TAG, "Tool confirmation timeout: %s", tool_ctx->tool_name);
                tool_ctx->confirmation_result = CONFIRMATION_TIMEOUT;
                tool_ctx->waiting_for_confirmation = false;
                
                // 发送超时通知
                mimi_msg_t timeout_msg = {0};
                strncpy(timeout_msg.channel, ctx->channel, sizeof(timeout_msg.channel) - 1);
                strncpy(timeout_msg.chat_id, ctx->chat_id, sizeof(timeout_msg.chat_id) - 1);
                timeout_msg.content = strdup("确认超时，操作已取消");
                if (timeout_msg.content) {
                    message_bus_push_outbound(&timeout_msg);
                }
                
                // 取消工具执行
                cancel_pending_tool(tool_ctx);
            }
        }
    }
    
    if (s_ctx_mutex) {
        mimi_mutex_unlock(s_ctx_mutex);
    }
}
```

### 5.5 继续Agent迭代

实现继续Agent迭代的函数：

```c
static void continue_agent_iteration(agent_request_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    
    // 检查是否还有待处理的工具
    // 这里需要根据实际的工具调用管理逻辑进行调整
    // 如果所有工具都已处理，继续LLM迭代
    
    // 重用per-request llm_response_t
    llm_response_t *next_resp = &ctx->llm_resp;
    memset(next_resp, 0, sizeof(*next_resp));
    
    mimi_err_t err = llm_chat_tools_async(ctx->system_prompt,
                                          ctx->messages,
                                          ctx->tools_json,
                                          next_resp,
                                          agent_llm_callback,
                                          ctx);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to start async LLM: %s", mimi_err_to_name(err));
        ctx->in_progress = false;
        if (s_ctx_mutex) {
            mimi_mutex_lock(s_ctx_mutex);
        }
        s_active_count--;
        if (s_ctx_mutex) {
            mimi_mutex_unlock(s_ctx_mutex);
        }
    }
}
```

## 6. 测试场景

| 场景 | 预期行为 |
|------|---------|
| 执行需要确认的工具 | 发送确认请求，等待用户响应 |
| 用户确认执行 | 执行工具并返回结果 |
| 用户设置总是允许 | 执行工具并将工具添加到总是允许列表 |
| 用户拒绝执行 | 取消工具执行并通知用户 |
| 用户超时未响应 | 自动取消操作并通知用户 |
| 执行已在总是允许列表的工具 | 直接执行，不发送确认请求 |

## 7. 代码修改点

### 7.1 新增文件

1. **control_manager.h** (`main/control/control_manager.h`)
   - 定义控制请求结构体`control_request_t`
   - 定义控制管理器API接口
   - 添加`control_manager_handle_response_by_chat_id()`函数
   - 添加`control_manager_generate_request_id()`函数

2. **control_manager.c** (`main/control/control_manager.c`)
   - 实现控制请求队列管理
   - 实现`control_manager_send_request()`
   - 实现`control_manager_handle_response()`
   - 实现`control_manager_handle_response_by_chat_id()`
   - 实现超时检查逻辑
   - 实现请求ID生成

3. **tool_call_context.h** (`main/tools/tool_call_context.h`)
   - 定义工具调用上下文结构体`tool_call_context_t`
   - 定义确认结果枚举`confirmation_result_t`
   - 定义工具调用上下文管理器API
   - 添加引用计数相关函数
   - 添加总是允许工具管理函数

4. **tool_call_context.c** (`main/tools/tool_call_context.c`)
   - 实现工具调用上下文队列管理
   - 实现`tool_call_context_create()`和`tool_call_context_destroy()`
   - 实现引用计数管理
   - 实现查找待确认工具上下文
   - 实现总是允许工具管理
   - 实现超时检查逻辑

### 7.2 修改文件

5. **tool_registry.h**：扩展工具结构，添加确认标志
6. **tool_registry.c**：修改工具注册，设置确认标志
7. **message_bus.h**：添加控制消息类型和相关字段
8. **message_bus.c**：修改消息处理逻辑，支持控制消息
9. **agent_async_loop.c**：
   - 移除工具调用上下文相关代码（迁移到新文件）
   - 集成控制管理器
   - 添加控制消息处理逻辑
   - 修改工具执行流程，使用新的工具调用上下文管理器
   - 添加`tool_confirm_callback`和`tool_confirm_execution_callback`函数
10. **channel.h**：添加处理控制消息的接口
11. **session_mgr.c**：添加总是允许工具的存储和加载
12. **cli_channel.c**：添加控制消息处理和确认提示
13. **cli_terminal.c**：添加控制响应检测和处理

### 7.3 文件依赖关系

```
agent_async_loop.c
    ├── control_manager.h
    │       └── message_bus.h
    ├── tool_call_context.h
    │       └── session_mgr.h
    ├── tool_registry.h
    └── llm/llm_proxy.h

control_manager.c
    ├── control_manager.h
    │       ├── message_bus.h
    │       └── mimi_err.h
    ├── os/mimi_os.h
    └── log.h

tool_call_context.c
    ├── tool_call_context.h
    │       ├── session_mgr.h
    │       └── mimi_err.h
    ├── os/mimi_os.h
    └── log.h

cli_channel.c
    ├── control_manager.h
    └── message_bus.h

cli_terminal.c
    ├── control_manager.h
    └── channel.h
```

## 8. 工具确认流程数据流向

### 8.1 泳道图

```
┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                            工具确认流程数据流向                                     │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────┘

┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│   用户 (User)    │  │  CLI Channel    │  │ Control Manager │  │ Agent Async Loop │
└────────┬─────────┘  └────────┬─────────┘  └────────┬─────────┘  └────────┬─────────┘
         │                     │                     │                     │
         │ 1. 发送消息        │                     │                     │
         ├─────────────────────▶│                     │                     │
         │                     │                     │                     │
         │                     │ 2. 推送到入队     │                     │
         │                     ├─────────────────────▶│                     │
         │                     │                     │                     │
         │                     │                     │ 3. 处理消息       │
         │                     │                     ├─────────────────────▶│
         │                     │                     │                     │
         │                     │                     │                     │ 4. 调用LLM
         │                     │                     │                     ├──────────────────▶
         │                     │                     │                     │              LLM API
         │                     │                     │                     │◀──────────────────
         │                     │                     │                     │
         │                     │                     │                     │ 5. 检测工具调用
         │                     │                     │                     │
         │                     │                     │                     │ 6. 创建工具上下文
         │                     │                     │                     │
         │                     │                     │                     │ 7. 发送确认请求
         │                     │                     │◀─────────────────────┤
         │                     │                     │                     │
         │                     │ 8. 推送控制请求   │                     │
         │                     │◀─────────────────────┤                     │
         │                     │                     │                     │
         │ 9. 显示确认提示    │                     │                     │
         │◀─────────────────────┤                     │                     │
         │                     │                     │                     │
         │ 10. 选择确认选项   │                     │                     │
         ├─────────────────────▶│                     │                     │
         │                     │                     │                     │
         │                     │ 11. 推送控制响应  │                     │
         │                     ├─────────────────────▶│                     │
         │                     │                     │                     │
         │                     │                     │ 12. 查找待处理请求 │
         │                     │                     │                     │
         │                     │                     │ 13. 调用回调       │
         │                     │                     │◀─────────────────────┤
         │                     │                     │                     │
         │                     │                     │                     │ 14. 处理确认结果
         │                     │                     │                     │
         │                     │                     │                     │ 15. 执行工具
         │                     │                     │                     ├──────────────────▶
         │                     │                     │                     │              Tool Registry
         │                     │                     │                     │◀──────────────────
         │                     │                     │                     │
         │                     │                     │                     │ 16. 工具执行完成
         │                     │                     │                     │
         │                     │                     │                     │ 17. 发送结果
         │                     │                     │                     ├──────────────────▶
         │                     │                     │                     │              Message Bus
         │                     │                     │                     │◀──────────────────
         │                     │                     │                     │
         │ 18. 显示执行结果    │                     │                     │
         │◀─────────────────────┤                     │                     │
         │                     │                     │                     │
└──────────────────┘  └──────────────────┘  └──────────────────┘  └──────────────────┘
```

### 8.2 详细流程说明

#### 阶段1：用户请求处理
1. **用户发送消息**：用户通过CLI终端输入消息
2. **CLI Channel接收**：CLI Channel接收用户消息并推送到入队
3. **Agent处理**：Agent Async Loop从入队中获取消息
4. **LLM调用**：Agent调用LLM API处理用户请求

#### 阶段2：工具调用检测
5. **检测工具调用**：Agent检测LLM响应中包含工具调用
6. **创建工具上下文**：Agent创建工具调用上下文，包含工具名称、参数等信息
7. **检查确认需求**：Agent检查工具是否需要用户确认
8. **发送确认请求**：如果需要确认，Agent通过Control Manager发送确认请求

#### 阶段3：确认请求展示
9. **推送控制请求**：Control Manager将控制请求推送到出队
10. **显示确认提示**：CLI Channel从出队中获取控制请求并显示确认提示
11. **用户选择**：用户选择确认选项（1=ACCEPT, 2=ACCEPT_ALWAYS, 3=REJECT）

#### 阶段4：确认响应处理
12. **推送控制响应**：CLI Channel将用户的选择作为控制响应推送到入队
13. **查找待处理请求**：Control Manager查找对应的待处理控制请求
14. **调用回调**：Control Manager调用注册的回调函数处理确认结果

#### 阶段5：工具执行
15. **处理确认结果**：Agent根据用户的确认结果决定是否执行工具
16. **执行工具**：如果用户确认，Agent通过Tool Registry执行工具
17. **工具执行完成**：Tool Registry完成工具执行并返回结果

#### 阶段6：结果返回
18. **发送结果**：Agent将工具执行结果推送到出队
19. **显示执行结果**：CLI Channel从出队中获取结果并显示给用户

### 8.3 数据流向特点

1. **异步处理**：所有操作都是异步的，不会阻塞主线程
2. **消息总线**：通过Message Bus实现组件间的解耦
3. **控制管理器**：统一管理所有控制请求和响应
4. **工具上下文**：维护工具调用的完整状态信息
5. **用户交互**：Channel负责与用户的交互界面

### 8.4 关键数据结构

#### 控制请求
```c
typedef struct {
    char request_id[64];           // 唯一请求ID
    char channel[16];             // 通道名称
    char chat_id[128];           // 聊天ID
    mimi_control_type_t control_type;  // 控制类型
    char target[64];              // 目标（如工具名称）
    void *context;               // 上下文指针
    uint64_t timeout;            // 超时时间
    void (*callback)(...);        // 回调函数
    bool active;                 // 是否活跃
} control_request_t;
```

#### 工具调用上下文
```c
typedef struct {
    char tool_name[64];          // 工具名称
    char tool_call_id[128];      // 工具调用ID
    char tool_input[1024];        // 工具输入
    void *agent_ctx;            // Agent上下文
    mimi_session_ctx_t session_ctx;  // 会话上下文
    bool requires_confirmation;   // 是否需要确认
    bool waiting_for_confirmation;  // 是否等待确认
    uint64_t confirmation_timeout;   // 确认超时
    confirmation_result_t confirmation_result;  // 确认结果
    bool executed;               // 是否已执行
    bool succeeded;             // 是否成功
    char output[32768];        // 输出结果
    mimi_err_t error_code;      // 错误代码
    void *async_ctx;           // 异步上下文
    void *user_data;           // 用户数据
    bool active;               // 是否活跃
    uint32_t ref_count;        // 引用计数
} tool_call_context_t;
```

## 9. 实现注意事项

1. **无侵入性**：保持现有工具执行流程不变，仅对需要确认的工具添加额外步骤
2. **异步处理**：使用消息总线和控制管理器实现异步控制流程
3. **用户友好**：控制消息清晰明了，包含足够信息让用户做出决策
4. **状态安全**：确保等待控制响应时的状态管理正确，避免内存泄漏或状态不一致
5. **超时处理**：合理处理用户未响应的情况，避免无限等待
6. **Channel兼容性**：确保不同Channel都能正确处理控制消息，根据自身特性渲染合适的控制界面
7. **上下文管理**：使用统一的工具调用上下文结构体，简化状态管理
8. **内存安全**：正确管理工具调用上下文和控制请求的生命周期，避免内存泄漏
9. **可扩展性**：设计控制消息格式时考虑未来可能的扩展需求
10. **一致性**：确保所有控制操作都通过控制管理器统一处理，保持架构一致性

## 9. 总结

通过本方案的实现，Mimiclaw框架将获得一个通用控制通道机制，不仅支持工具调用的用户确认，还支持停止操作、状态查询等多种控制场景。这种设计具有以下优势：

1. **通用性**：适用于多种控制场景，不仅仅是工具确认
2. **可扩展性**：可以轻松添加新的控制类型
3. **统一接口**：所有控制操作都通过同一个通道处理
4. **异步处理**：使用回调机制处理响应，不阻塞主线程
5. **状态管理**：集中管理控制请求的状态和超时
6. **Channel无关性**：控制机制对所有Channel透明，无需Channel-specific代码

通过消息总线和控制管理器的配合，实现了安全、流畅的用户交互流程，同时保持了架构的一致性和可扩展性。统一的工具调用上下文结构体设计，使得状态管理更加清晰和易于维护。