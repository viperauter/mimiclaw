cmake_minimum_required(VERSION 3.16)
project(mimiclaw C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Windows: static link pthread to avoid DLL dependency
if(WIN32)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()

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
    main/platform/path_utils.c

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
    main/proxy/http_proxy.c
    main/cli/editor.c
    main/cli/cli_terminal.c

    # Commands system (shared across channels)
    main/commands/command_registry.c
    main/commands/cmd_help.c
    main/commands/cmd_session.c
    main/commands/cmd_set.c
    main/commands/cmd_memory.c
    main/commands/cmd_ask.c
    main/commands/cmd_exit.c

    # Channels system
    main/channels/channel_manager.c
    main/channels/cli/cli_channel.c
    main/channels/cli/terminal_stdio.c
    main/channels/telegram/telegram_channel.c
    main/channels/websocket/ws_channel.c
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
    pthread
)

# Windows-specific libraries
if(WIN32)
    target_link_libraries(mimiclaw PRIVATE ws2_32)
endif()