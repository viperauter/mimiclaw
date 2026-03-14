cmake_minimum_required(VERSION 3.16)

# --------------------------------
# FreeRTOS POSIX runtime library (simulator)
# --------------------------------
#
# Builds a standalone FreeRTOS runtime that can run as a normal host process
# using the POSIX (pthread/signal) portable layer.
#
option(FREERTOS_BUILD_RUNTIME "Build FreeRTOS POSIX runtime library" ON)

if(FREERTOS_BUILD_RUNTIME)
    set(FREERTOS_IMPL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS/Source")
    set(FREERTOS_POSIX_PORT_DIR "${FREERTOS_IMPL_DIR}/portable/ThirdParty/GCC/Posix")
    set(FREERTOS_INCLUDE_DIR "${FREERTOS_IMPL_DIR}/include")
    # Project-local FreeRTOSConfig.h lives one level above Source/.
    set(FREERTOS_PROJECT_CFG_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

    if(NOT EXISTS "${FREERTOS_INCLUDE_DIR}/FreeRTOS.h")
        message(FATAL_ERROR "FreeRTOS headers not found: ${FREERTOS_INCLUDE_DIR}/FreeRTOS.h")
    endif()
    if(NOT EXISTS "${FREERTOS_POSIX_PORT_DIR}/port.c")
        message(FATAL_ERROR "FreeRTOS POSIX port not found: ${FREERTOS_POSIX_PORT_DIR}/port.c")
    endif()

    add_library(freertos_runtime_posix STATIC
        ${FREERTOS_IMPL_DIR}/event_groups.c
        ${FREERTOS_IMPL_DIR}/list.c
        ${FREERTOS_IMPL_DIR}/queue.c
        ${FREERTOS_IMPL_DIR}/stream_buffer.c
        ${FREERTOS_IMPL_DIR}/tasks.c
        ${FREERTOS_IMPL_DIR}/timers.c

        ${FREERTOS_POSIX_PORT_DIR}/port.c
        ${FREERTOS_POSIX_PORT_DIR}/utils/wait_for_event.c

        ${FREERTOS_IMPL_DIR}/portable/MemMang/heap_3.c

        ${CMAKE_CURRENT_SOURCE_DIR}/freertos_hooks.c
    )

    # FreeRTOS headers must come BEFORE project headers to avoid conflicts
    # (e.g., queue.h in FreeRTOS vs queue.h in main/core/platform/)
    target_include_directories(freertos_runtime_posix BEFORE PUBLIC
        "${FREERTOS_PROJECT_CFG_DIR}"
        "${FREERTOS_INCLUDE_DIR}"
        "${FREERTOS_POSIX_PORT_DIR}"
        "${FREERTOS_POSIX_PORT_DIR}/utils"
    )
    target_include_directories(freertos_runtime_posix PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.."
    )

    target_link_libraries(freertos_runtime_posix PUBLIC pthread)
endif()



# --------------------------------
# Demos
# --------------------------------

# Simple FreeRTOS demo: create a task that toggles/logs periodically.
option(FREERTOS_BUILD_DEMO "Build simple FreeRTOS demo executable" ON)

if(FREERTOS_BUILD_DEMO)
    if(NOT TARGET freertos_runtime_posix)
        message(FATAL_ERROR "freertos_demo requires freertos_runtime_posix")
    endif()

    add_executable(freertos_demo
        demo_freertos_main.c
    )
    target_link_libraries(freertos_demo PRIVATE freertos_runtime_posix)
endif()



