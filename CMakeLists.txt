cmake_minimum_required(VERSION 3.16)
project(mimiclaw C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --------------------------------
# compiler options
# --------------------------------
include(cmake/compiler_options.cmake)

# --------------------------------
# third party
# --------------------------------
include(cmake/third_party.cmake)

# --------------------------------
# source files
# --------------------------------
file(GLOB_RECURSE APP_SRC
    main/mimi.c
    main/config/config.c
    main/config/workspace_bootstrap.c

    # Platform implementation (POSIX)
    main/platform/mimi_err.c
    main/platform/log.c
    main/platform/mimi_time.c
    main/platform/random.c
    main/platform/os/posix_impl/os_posix.c
    main/platform/runtime.c
    main/platform/queue.c
    main/platform/fs/fs.c
    main/platform/fs/posix_impl/fs_posix.c
    main/platform/kv.c
    main/platform/http/mg_impl/http.c

    # Core app modules (platform-agnostic)
    main/bus/message_bus.c
    main/memory/memory_store.c
    main/memory/session_mgr.c
    main/skills/skill_loader.c
    main/llm/llm_proxy.c
    main/tools/tool_registry.c
    main/tools/tool_files.c
    main/tools/tool_cron.c
    main/tools/tool_web_search.c
    main/tools/tool_get_time.c
    main/cron/cron_service.c
    main/heartbeat/heartbeat.c
    main/agent/context_builder.c
    main/agent/agent_loop.c
    main/gateway/ws_server.c
    main/telegram/telegram_bot.c
    main/proxy/http_proxy.c
    main/cli/cli_core.c
    main/cli/stdio_cli_posix.c
)

# --------------------------------
# executable
# --------------------------------
add_executable(mimiclaw
    ${APP_SRC}
)

# --------------------------------
# include dirs
# --------------------------------
target_include_directories(mimiclaw PRIVATE
    main
    main/platform
    main/config
    third_party
)

# --------------------------------
# link libraries
# --------------------------------
target_link_libraries(mimiclaw
    PRIVATE
    mongoose
    cjson
    linenoise
    pthread
)