cmake_minimum_required(VERSION 3.16)
project(mimiclaw C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

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
# Module reorganization
# Core modules are moved to main/core/ for better organization
# Include directories are set up to maintain backward compatibility
# with existing #include statements in source files
# --------------------------------
include_directories(
    ${CMAKE_SOURCE_DIR}/main
    ${CMAKE_SOURCE_DIR}/main/core
    ${CMAKE_SOURCE_DIR}/main/core/platform
    ${CMAKE_SOURCE_DIR}/main/core/platform/os
    ${CMAKE_SOURCE_DIR}/main/core/config
    ${CMAKE_SOURCE_DIR}/main/services
    ${CMAKE_SOURCE_DIR}/main/interface
    ${CMAKE_SOURCE_DIR}/main/agent
    ${CMAKE_SOURCE_DIR}/third_party
)

# --------------------------------
# source files
# --------------------------------
set(APP_SRC
    main/main.c
    main/app/app.c

    # Core modules (moved to core/ directory)
    main/core/config/config.c
    main/core/config/workspace_bootstrap.c
    main/core/bus/message_bus.c
    main/core/platform/mimi_err.c
    main/core/platform/log.c
    main/core/platform/random.c
    main/core/platform/runtime.c
    main/core/platform/queue.c
    main/core/platform/event/io_buf.c
    main/core/platform/event/event_bus.c
    main/core/platform/event/event_dispatcher.c
    main/core/platform/fs/fs.c
    main/core/platform/kv.c
    main/core/platform/http/mg_impl/http.c
    main/core/platform/websocket/mg_impl/ws_client.c
    main/core/platform/websocket/mg_impl/ws_server.c
    main/core/platform/path_utils.c
)

# OS backend specific files
if(OS_BACKEND STREQUAL "posix")
    set(OS_IMPL_FILES
        main/core/platform/os/posix_impl/os_posix.c
    )
elseif(OS_BACKEND STREQUAL "freertos")
    set(OS_IMPL_FILES
        main/core/platform/os/freertos_impl/os_freertos.c
    )
else()
    message(FATAL_ERROR "Invalid OS_BACKEND: ${OS_BACKEND}")
endif()

# Always include the POSIX filesystem implementation for now
list(APPEND OS_IMPL_FILES
    main/core/platform/fs/posix_impl/fs_posix.c
)

list(APPEND APP_SRC ${OS_IMPL_FILES})

list(APPEND APP_SRC
    # Memory management
    main/memory/memory_store.c
    main/memory/session_mgr.c

    # Skills system
    main/skills/skill_loader.c

    # Services (moved to services/ directory)
    main/services/llm/llm_proxy.c
    main/services/cron/cron_service.c
    main/services/heartbeat/heartbeat.c
    main/services/proxy/http_proxy.c

    # Agent modules (including control and tools)
    main/agent/context_builder.c
    main/agent/agent_async.c
    main/agent/agent_async_loop.c
    main/agent/control/control_manager.c
    main/agent/tools/tool_registry.c
    main/agent/tools/tool_files.c
    main/agent/tools/tool_cron.c
    main/agent/tools/tool_web_search.c
    main/agent/tools/tool_get_time.c
    main/agent/tools/tool_call_context.c

    # Interface modules (moved to interface/ directory)
    main/interface/cli/editor.c
    main/interface/cli/cli_terminal.c
    main/interface/commands/command_registry.c
    main/interface/commands/cmd_help.c
    main/interface/commands/cmd_session.c
    main/interface/commands/cmd_set.c
    main/interface/commands/cmd_memory.c
    main/interface/commands/cmd_ask.c
    main/interface/commands/cmd_exit.c
    main/interface/router/router.c

    # Gateway layer
    main/gateway/gateway.c
    main/gateway/gateway_manager.c
    main/gateway/stdio/stdio_gateway.c
    main/gateway/websocket/ws_server_gateway.c
    main/gateway/websocket/ws_client_gateway.c
    main/gateway/http/http_client_gateway.c

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
# include dirs (for target-specific includes)
# --------------------------------
target_include_directories(mimiclaw PRIVATE
    main
    main/app
    main/core/platform
    main/core/config
    third_party
)

# --------------------------------
# link libraries
# --------------------------------
target_link_libraries(mimiclaw
    PRIVATE
    mongoose
    OpenSSL::SSL
    OpenSSL::Crypto
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

# Only add FreeRTOS subdirectory when using FreeRTOS backend
if(OS_BACKEND STREQUAL "freertos")
    add_subdirectory(main/core/platform/os/freertos_impl)
endif()
