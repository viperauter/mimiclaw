cmake_minimum_required(VERSION 3.16)
project(mimiclaw C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Use builtin TLS in Mongoose for https:// APIs
add_compile_definitions(MG_TLS=MG_TLS_BUILTIN)

add_executable(mimiclaw
  main/main_posix.c

  # Platform implementation (POSIX)
  main/platform/posix_impl/mimi_err_posix.c
  main/platform/posix_impl/log_posix.c
  main/platform/posix_impl/time_posix.c
  main/platform/posix_impl/random_posix.c
  main/platform/posix_impl/os_posix.c
  main/platform/posix_impl/queue_posix.c
  main/platform/posix_impl/fs_posix.c
  main/platform/posix_impl/kv_posix.c
  main/platform/posix_impl/http_posix.c

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
  main/cli/cli_core.c
  main/cli/stdio_cli_posix.c

  # Third-party libs
  third_party/mongoose/mongoose.c
  third_party/cJSON/cJSON.c
)

target_include_directories(mimiclaw PRIVATE
  main
  third_party/mongoose
  third_party/cJSON
)

target_link_libraries(mimiclaw PRIVATE pthread)

