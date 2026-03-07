# CLI 功能说明

## 概述

本 CLI 库是一个跨平台的命令行界面实现，采用插件架构设计，支持多种终端类型，包括标准输入、WebSocket、串口和 TCP。它提供了命令执行、Tab 补全、历史记录等功能，为用户提供友好的交互体验。

## 功能特性

### 核心功能

1. **命令执行**：支持执行以 `/` 开头的命令，如 `/help`、`/session` 等
2. **Tab 补全**：输入命令前缀后按 Tab 键自动补全
3. **历史记录**：支持上下箭头浏览历史命令
4. **多终端支持**：可同时处理不同类型的终端，如标准输入、WebSocket、串口和 TCP
5. **跨平台兼容**：支持 Windows、Linux 和 macOS
6. **插件架构**：通过 transport 接口轻松扩展新的终端类型

### 支持的命令

- `/help` - 列出所有可用命令
- `/session <list|new|use|clear>` - 管理会话
- `/set <api_key|model|model_provider|tg_token|search_key> <value>` - 设置配置
- `/memory_read` - 打印 MEMORY.md
- `/ask <channel> <chat_id> <text...>` - 向代理注入消息
- `/exit` - 退出应用

## 架构设计

### 分层架构

CLI 库采用清晰的分层架构，从底层到应用层依次为：

```
┌─────────────────────────────────────────┐
│  应用层 (terminal_stdio.c)             │
│  - 实现 transport 接口                  │
│  - 只关心字节流的读写                     │
└─────────────────┬───────────────────────┘
                  │ cli_transport_t 接口
                  ▼
┌─────────────────────────────────────────┐
│  应用框架层 (cli_terminal.c/h)         │
│  - app_terminal_create/destroy           │
│  - app_terminal_poll_all()               │
│  - 命令执行、session管理                  │
│  - 与 editor 库交互                       │
└─────────────────┬───────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────┐
│  行编辑器层 (editor.c/h)              │
│  - 光标移动、历史记录、Tab补全             │
│  - 多终端管理                           │
└─────────────────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────┐
│  命令系统层 (command.c/h)            │
│  - 命令注册/注销                        │
│  - 命令执行                             │
│  - 命令补全（返回匹配列表）               │
└─────────────────────────────────────────┘
```

### Transport 接口

`cli_transport_t` 是终端类型的抽象接口，定义了所有终端必须实现的方法：

```c
typedef struct {
    int (*read)(void *ctx, char *buf, int len);      /* 读取数据 */
    int (*write)(void *ctx, const char *buf, int len); /* 写入数据 */
    bool (*available)(void *ctx);                       /* 检查是否有数据可读 */
    void (*close)(void *ctx);                          /* 关闭终端 */
    void *ctx;                                         /* 终端特定上下文 */
} cli_transport_t;
```

### 核心组件

#### 1. 行编辑器层 (editor.c/h)

- **职责**：提供行编辑功能，包括光标移动、字符插入/删除、历史记录、Tab 补全
- **关键函数**：
  - `cli_init()` - 初始化编辑器
  - `cli_terminal_create()` - 创建编辑器终端实例
  - `cli_poll()` - 轮询输入并处理
  - `cli_run()` - 阻塞式主循环

#### 2. 命令系统层 (command.c/h)

- **职责**：管理命令注册、执行和补全
- **关键函数**：
  - `cli_core_init()` - 初始化命令系统
  - `cli_register_cmd()` - 注册新命令
  - `cli_core_execute_line()` - 执行命令行
  - `cli_get_completions()` - 获取补全匹配

#### 3. 应用框架层 (cli_terminal.c/h)

- **职责**：提供通用的终端管理框架，整合编辑器和命令系统
- **关键函数**：
  - `app_terminal_init()` - 初始化框架
  - `app_terminal_create()` - 创建应用终端
  - `app_terminal_destroy()` - 销毁应用终端
  - `app_terminal_poll_all()` - 轮询所有终端

#### 4. 终端实现层 (terminal_stdio.c)

- **职责**：实现具体的传输层（如 stdio）
- **关键函数**：
  - `stdio_read()` - 从标准输入读取
  - `stdio_write()` - 写入到标准输出
  - `stdio_available()` - 检查是否有输入
  - `stdio_close()` - 关闭标准输入

## 使用指南

### app_terminal_poll_all() 的用法

`app_terminal_poll_all()` 是应用框架层的核心函数，用于轮询所有活动终端的输入。

#### 函数签名

```c
void app_terminal_poll_all(void);
```

#### 功能说明

1. **轮询所有终端**：遍历所有已创建的终端实例
2. **检查可用数据**：调用每个终端的 `available()` 函数检查是否有输入
3. **读取数据**：如果有数据，调用 `read()` 函数读取
4. **处理输入**：将读取的数据通过 `cli_terminal_feed_char()` 喂给编辑器
5. **错误处理**：如果读取失败，自动销毁该终端

#### 调用时机

`app_terminal_poll_all()` 应该在主循环中定期调用，通常间隔 10-50ms：

```c
/* 示例：在主循环中调用 */
while (!should_exit) {
    app_terminal_poll_all();  /* 轮询所有终端 */
    
    /* 处理其他任务 */
    do_other_tasks();
    
    mimi_sleep_ms(10);  /* 休眠 10ms */
}
```

#### 多终端场景

当有多个终端时（如 stdio + TCP），`app_terminal_poll_all()` 会自动处理所有终端的输入：

```c
/* 创建 stdio 终端 */
app_terminal_t *stdio_term = create_stdio_terminal();

/* 创建 TCP 终端 */
app_terminal_t *tcp_term = create_tcp_terminal();

/* 主循环 - 同时处理两个终端 */
while (!should_exit) {
    app_terminal_poll_all();  /* 自动处理 stdio 和 TCP 的输入 */
    mimi_sleep_ms(10);
}
```

#### 注意事项

1. **非阻塞**：`app_terminal_poll_all()` 是非阻塞的，如果没有数据会立即返回
2. **自动清理**：如果终端读取失败，会自动从列表中移除并销毁
3. **线程安全**：每个终端使用独立的上下文，可以安全地在多线程环境中使用
4. **性能考虑**：调用频率不宜过高，建议 10-50ms 间隔

## 编译方法

### 前提条件

- CMake 3.16 或更高版本
- C 编译器（如 GCC、Clang 或 MSVC）
- 依赖库：
  - mongoose（网络库）
  - cjson（JSON 解析库）
  - microrl（轻量级 Readline 替代库）

### 编译步骤

#### Windows 交叉编译（从 Linux/macOS）

1. **配置 CMake**
   ```bash
   cmake -B build -S . -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc.exe -DCMAKE_SYSTEM_NAME=Windows
   ```

2. **编译项目**
   ```bash
   cmake --build build --config Release
   ```

3. **运行可执行文件**
   ```bash
   ./build/mimiclaw.exe
   ```

#### 本地编译

1. **配置 CMake**
   ```bash
   cmake -B build -S .
   ```

2. **编译项目**
   ```bash
   cmake --build build --config Release
   ```

3. **运行可执行文件**
   ```bash
   ./build/mimiclaw.exe  # Windows
   ./build/mimiclaw      # Linux/macOS
   ```

## 测试方法

### 基本功能测试

1. **启动 CLI**
   - 运行可执行文件，应该看到提示符：`[HH:MM:SS] mimiclaw(cli:default)>`

2. **测试命令执行**
   - 输入 `/help` 并按回车，应该显示所有可用命令
   - 输入 `/session list` 并按回车，应该显示会话列表
   - 输入 `/memory_read` 并按回车，应该显示 MEMORY.md 内容

3. **测试 Tab 补全**
   - 输入 `/h` 并按 Tab 键，应该补全为 `/help`
   - 输入 `/s` 并按 Tab 键，应该显示所有以 `/s` 开头的命令（如 `/session`、`/set`）
   - 输入 `/se` 并按 Tab 键，应该补全为 `/session`

4. **测试历史记录**
   - 输入几个命令，然后按上箭头键，应该可以浏览历史命令

### 高级功能测试

1. **多终端测试**
   - 启动标准输入终端
   - 同时连接 WebSocket 终端
   - 验证两个终端可以独立工作，互不干扰

2. **命令参数测试**
   - 测试 `/set` 命令设置不同配置
   - 测试 `/session new <chat_id>` 创建新会话
   - 测试 `/session use <chat_id>` 切换会话

## 预期结果

### 命令执行

- 输入 `/help` 应该显示：
  ```
  Available commands:
    help: list commands
    exit: quit the application
    set <api_key|model|model_provider|tg_token|search_key> <value>
    memory_read: print MEMORY.md
    session <list|clear> [...]
    ask <channel> <chat_id> <text...>: inject a message into agent
  ```

- 输入 `/session list` 应该显示会话列表：
  ```
  Sessions (channel=default):
    - default_test.jsonl
  Total: 1 session(s)
  ```

### Tab 补全

- 输入 `/h` + Tab 应该补全为 `/help`
- 输入 `/s` + Tab 应该显示：
  ```
  session  set
  ```
- 输入 `/se` + Tab 应该补全为 `/session`

### 历史记录

- 按上箭头键应该显示之前输入的命令
- 按下箭头键应该显示之后输入的命令

## 故障排除

### 常见问题

1. **Tab 补全不工作**
   - 确保终端支持 Tab 键输入
   - 检查 `cli_complete` 函数是否正确注册

2. **命令执行失败**
   - 检查命令拼写是否正确
   - 检查命令参数是否符合要求

3. **乱码问题**
   - 在 Windows 上，确保控制台编码设置为 UTF-8
   - 在 Linux/macOS 上，确保终端编码设置为 UTF-8

4. **多终端冲突**
   - 确保每个终端使用独立的上下文
   - 检查终端初始化和销毁逻辑

5. **app_terminal_poll_all() 无响应**
   - 检查是否在主循环中定期调用
   - 检查终端的 `available()` 函数是否正确实现
   - 检查终端的 `read()` 函数是否正确返回数据

## 代码结构

- `editor.c/h` - 行编辑器实现，包含光标管理、历史记录、Tab 补全
- `command.c/h` - 命令系统实现，处理命令注册和执行
- `cli_terminal.c/h` - 应用框架层，提供通用的终端管理
- `terminal_stdio.c` - 标准输入终端实现

### 扩展新终端类型

要添加新的终端类型（如 TCP、WebSocket 等），只需实现 `cli_transport_t` 接口：

#### 示例：添加 TCP 终端

```c
/* terminal_tcp.c */
#include "cli/cli_terminal.h"
#include <sys/socket.h>

typedef struct {
    int socket_fd;
} tcp_ctx_t;

/* 实现 transport 接口 */
static int tcp_read(void *ctx, char *buf, int len) {
    tcp_ctx_t *tcp = (tcp_ctx_t *)ctx;
    return recv(tcp->socket_fd, buf, len, 0);
}

static int tcp_write(void *ctx, const char *buf, int len) {
    tcp_ctx_t *tcp = (tcp_ctx_t *)ctx;
    return send(tcp->socket_fd, buf, len, 0);
}

static bool tcp_available(void *ctx) {
    tcp_ctx_t *tcp = (tcp_ctx_t *)ctx;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(tcp->socket_fd, &fds);
    struct timeval tv = {0, 0};
    return select(tcp->socket_fd + 1, &fds, NULL, NULL, &tv) > 0;
}

static void tcp_close(void *ctx) {
    tcp_ctx_t *tcp = (tcp_ctx_t *)ctx;
    close(tcp->socket_fd);
    free(tcp);
}

/* 创建 TCP 终端 */
app_terminal_t* create_tcp_terminal(const char *host, int port) {
    /* 创建 socket 并连接 */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    /* ... 连接逻辑 ... */
    
    /* 创建 transport 接口 */
    tcp_ctx_t *ctx = malloc(sizeof(tcp_ctx_t));
    ctx->socket_fd = sock;
    
    cli_transport_t transport = {
        .read = tcp_read,
        .write = tcp_write,
        .available = tcp_available,
        .close = tcp_close,
        .ctx = ctx
    };
    
    /* 创建应用终端 */
    app_terminal_config_t config = {
        .name = "tcp",
        .channel = "tcp",
        .chat_id = "default",
        .transport = transport
    };
    
    return app_terminal_create(&config);
}
```

#### 使用新终端

```c
/* 在主程序中 */
app_terminal_init();

/* 创建 stdio 终端 */
app_terminal_t *stdio_term = create_stdio_terminal();

/* 创建 TCP 终端 */
app_terminal_t *tcp_term = create_tcp_terminal("192.168.1.1", 8080);

/* 主循环 - 同时处理两个终端 */
while (!should_exit) {
    app_terminal_poll_all();  /* 自动处理所有终端的输入 */
    mimi_sleep_ms(10);
}

/* 清理 */
app_terminal_destroy(stdio_term);
app_terminal_destroy(tcp_term);
```

## 扩展指南

### 添加新命令

1. 在 `command.c` 中定义命令处理函数
2. 在 `register_builtin_commands` 函数中注册新命令
3. 实现命令逻辑并返回适当的错误码

### 添加新终端类型

1. 创建新的终端实现文件（如 `terminal_tcp.c`）
2. 实现 `cli_transport_t` 接口的四个函数
3. 创建终端配置并调用 `app_terminal_create()`
4. 在主程序中初始化和启动新终端

## 总结

本 CLI 库采用插件架构设计，提供了一个功能完整、跨平台的命令行界面。通过 `cli_transport_t` 接口，可以轻松支持多种终端类型。`app_terminal_poll_all()` 函数简化了多终端的管理，使得应用可以同时处理多个输入源。通过遵循本文档的编译和测试方法，您可以确保 CLI 功能正常工作，并根据需要进行扩展和定制。
