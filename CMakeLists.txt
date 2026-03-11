cmake_minimum_required(VERSION 3.16)
project(mimiclaw C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --------------------------------
# OS backend selection
# --------------------------------
set(OS_BACKEND "posix" CACHE STRING "OS backend to use (posix or freertos)")
set_property(CACHE OS_BACKEND PROPERTY STRINGS posix freertos)

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
set(APP_SRC
    main/main.c
    main/app/app.c
    main/config/config.c
    main/config/workspace_bootstrap.c

    # Platform implementation
    main/platform/mimi_err.c
    main/platform/log.c
    main/platform/random.c
    main/platform/runtime.c
    main/platform/queue.c
    main/platform/event/io_buf.c
    main/platform/event/event_bus.c
    main/platform/event/event_dispatcher.c
    main/platform/fs/fs.c
    main/platform/kv.c
    main/platform/http/mg_impl/http.c
    main/platform/websocket/mg_impl/ws_client.c
    main/platform/websocket/mg_impl/ws_server.c
    main/platform/path_utils.c
)

# OS backend specific files
if(OS_BACKEND STREQUAL "posix")
    set(OS_IMPL_FILES
        main/platform/os/posix_impl/os_posix.c
    )
elseif(OS_BACKEND STREQUAL "freertos")
    set(OS_IMPL_FILES
        main/platform/os/freertos_impl/os_freertos.c
    )
else()
    message(FATAL_ERROR "Invalid OS_BACKEND: ${OS_BACKEND}")
endif()

# Always include the POSIX filesystem implementation for now
list(APPEND OS_IMPL_FILES
    main/platform/fs/posix_impl/fs_posix.c
)

list(APPEND APP_SRC ${OS_IMPL_FILES})

list(APPEND APP_SRC
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

    # Gateway layer
        main/gateway/gateway.c
        main/gateway/gateway_manager.c
        main/gateway/stdio/stdio_gateway.c
        main/gateway/websocket/ws_server_gateway.c
        main/gateway/websocket/ws_client_gateway.c
        main/gateway/http/http_client_gateway.c

    # Input processor
    main/router/router.c

    # Channels system
    main/channels/channel_manager.c
    main/channels/cli/cli_channel.c
    main/channels/telegram/telegram_channel.c
    main/channels/ws_service/ws_service_channel.c
    main/channels/feishu/feishu_channel.c
    main/channels/feishu/feishu_media.c
    main/channels/qq/qq_channel.c
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
    main/app
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
    nanopb
    feishu_pb
    pthread
)

# OS backend specific dependencies
if(OS_BACKEND STREQUAL "freertos")
    target_link_libraries(mimiclaw PRIVATE freertos_runtime_posix)
    target_compile_definitions(mimiclaw PRIVATE OS_BACKEND_FREERTOS)
elseif(OS_BACKEND STREQUAL "posix")
    target_compile_definitions(mimiclaw PRIVATE OS_BACKEND_POSIX)
endif()

# Windows-specific libraries
if(WIN32)
    target_link_libraries(mimiclaw PRIVATE ws2_32)
endif()

add_subdirectory(main/platform/os/freertos_impl)