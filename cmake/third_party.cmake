include(FetchContent)

set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/third_party)

# =====================================================
# mongoose
# =====================================================
set(MONGOOSE_DIR ${THIRD_PARTY_DIR}/mongoose)

# Prefer a complete local checkout; otherwise populate via FetchContent.
if(EXISTS ${MONGOOSE_DIR}/mongoose.c)
    message(STATUS "Using cached mongoose source: ${MONGOOSE_DIR}")
    set(mongoose_SOURCE_DIR ${MONGOOSE_DIR})
else()
    message(STATUS "Fetching mongoose from GitHub...")
    FetchContent_Declare(
        mongoose
        GIT_REPOSITORY https://gitee.com/mirrors/mongoose.git
        GIT_TAG 7.14
        SOURCE_DIR ${MONGOOSE_DIR}
    )
    FetchContent_Populate(mongoose)
endif()

add_library(mongoose STATIC
    ${mongoose_SOURCE_DIR}/mongoose.c
)

target_include_directories(mongoose PUBLIC
    ${mongoose_SOURCE_DIR})

# =====================================================
# cJSON
# =====================================================

set(CJSON_DIR ${THIRD_PARTY_DIR}/cjson)

# Prefer a complete local checkout; otherwise populate via FetchContent.
if(EXISTS ${CJSON_DIR}/cJSON.c)
    message(STATUS "Using cached cJSON source: ${CJSON_DIR}")
    set(cjson_SOURCE_DIR ${CJSON_DIR})
else()
    message(STATUS "Fetching cJSON from GitHub...")
    FetchContent_Declare(
        cjson
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG v1.7.18
        SOURCE_DIR ${CJSON_DIR}
    )
    FetchContent_Populate(cjson)
endif()

add_library(cjson STATIC
    ${cjson_SOURCE_DIR}/cJSON.c
)

target_include_directories(cjson PUBLIC
    ${cjson_SOURCE_DIR})

# =====================================================
# nanopb - Protocol Buffers for Embedded Systems
# =====================================================

set(NANOPB_DIR ${THIRD_PARTY_DIR}/nanopb)

# Prefer a complete local checkout; otherwise populate via FetchContent.
if(EXISTS ${NANOPB_DIR}/pb_encode.c AND EXISTS ${NANOPB_DIR}/generator/nanopb_generator.py)
    message(STATUS "Using cached nanopb source: ${NANOPB_DIR}")
    set(nanopb_SOURCE_DIR ${NANOPB_DIR})
else()
    message(STATUS "Fetching nanopb from GitHub...")
    FetchContent_Declare(
        nanopb
        GIT_REPOSITORY https://github.com/nanopb/nanopb.git
        GIT_TAG 0.4.9.1
        SOURCE_DIR ${NANOPB_DIR}
    )
    FetchContent_Populate(nanopb)
endif()

add_library(nanopb STATIC
    ${nanopb_SOURCE_DIR}/pb_encode.c
    ${nanopb_SOURCE_DIR}/pb_decode.c
    ${nanopb_SOURCE_DIR}/pb_common.c
)

target_include_directories(nanopb PUBLIC
    ${nanopb_SOURCE_DIR}
    ${nanopb_SOURCE_DIR}/include)

# =====================================================
# Feishu Protobuf Generation
# =====================================================

set(FEISHU_PB_DIR ${CMAKE_SOURCE_DIR}/main/channels/feishu/pb)
set(FEISHU_PB_PROTO ${FEISHU_PB_DIR}/pbbp2.proto)
set(FEISHU_PB_HEADER ${FEISHU_PB_DIR}/pbbp2.pb.h)
set(FEISHU_PB_SOURCE ${FEISHU_PB_DIR}/pbbp2.pb.c)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

option(MIMICLAW_FEISHU_PB_USE_VENV "Create venv in third_party dir to run nanopb generator" ON)

set(FEISHU_PB_PYTHON_EXECUTABLE ${Python3_EXECUTABLE})

if(MIMICLAW_FEISHU_PB_USE_VENV)
    set(FEISHU_PB_VENV_DIR ${THIRD_PARTY_DIR}/feishu_pb_venv)
    if(WIN32)
        set(FEISHU_PB_VENV_PY ${FEISHU_PB_VENV_DIR}/Scripts/python.exe)
    else()
        set(FEISHU_PB_VENV_PY ${FEISHU_PB_VENV_DIR}/bin/python)
    endif()

    if(NOT EXISTS ${FEISHU_PB_VENV_PY})
        message(STATUS "Creating venv for Feishu protobuf generator: ${FEISHU_PB_VENV_DIR}")
        execute_process(
            COMMAND ${Python3_EXECUTABLE} -m venv ${FEISHU_PB_VENV_DIR}
            RESULT_VARIABLE _feishu_pb_venv_rc
        )
        if(NOT _feishu_pb_venv_rc EQUAL 0)
            message(FATAL_ERROR "Failed to create venv for Feishu protobuf generator (rc=${_feishu_pb_venv_rc}). Disable with -DMIMICLAW_FEISHU_PB_USE_VENV=OFF")
        endif()

        execute_process(
            COMMAND ${FEISHU_PB_VENV_PY} -m pip install --upgrade pip
            RESULT_VARIABLE _feishu_pb_pip_upgrade_rc
        )
        if(NOT _feishu_pb_pip_upgrade_rc EQUAL 0)
            message(FATAL_ERROR "Failed to upgrade pip in Feishu protobuf venv (rc=${_feishu_pb_pip_upgrade_rc}). Disable with -DMIMICLAW_FEISHU_PB_USE_VENV=OFF")
        endif()

        execute_process(
            COMMAND ${FEISHU_PB_VENV_PY} -m pip install protobuf grpcio-tools
            RESULT_VARIABLE _feishu_pb_pip_install_rc
        )
        if(NOT _feishu_pb_pip_install_rc EQUAL 0)
            message(FATAL_ERROR "Failed to install python deps (protobuf grpcio-tools) for Feishu protobuf generator (rc=${_feishu_pb_pip_install_rc}). Disable with -DMIMICLAW_FEISHU_PB_USE_VENV=OFF")
        endif()
    endif()

    set(FEISHU_PB_PYTHON_EXECUTABLE ${FEISHU_PB_VENV_PY})
endif()

add_custom_command(
    OUTPUT ${FEISHU_PB_HEADER} ${FEISHU_PB_SOURCE}
    COMMAND ${FEISHU_PB_PYTHON_EXECUTABLE}
        ${nanopb_SOURCE_DIR}/generator/nanopb_generator.py
        -I ${FEISHU_PB_DIR}
        ${FEISHU_PB_PROTO}
    DEPENDS ${FEISHU_PB_PROTO} ${nanopb_SOURCE_DIR}/generator/nanopb_generator.py
    WORKING_DIRECTORY ${FEISHU_PB_DIR}
    COMMENT "Generating Feishu protobuf files"
)

add_custom_target(feishu_pb_gen
    DEPENDS ${FEISHU_PB_HEADER} ${FEISHU_PB_SOURCE}
)

add_library(feishu_pb STATIC
    ${FEISHU_PB_SOURCE}
)

add_dependencies(feishu_pb feishu_pb_gen)

target_include_directories(feishu_pb PUBLIC
    ${FEISHU_PB_DIR}
    ${nanopb_SOURCE_DIR}/include
)

target_link_libraries(feishu_pb PUBLIC nanopb)

# =====================================================
# lowdown - Markdown parser for terminal rendering
# =====================================================
option(MIMICLAW_ENABLE_LOWDOWN "Enable lowdown markdown rendering for STDIO" ON)

if(MIMICLAW_ENABLE_LOWDOWN)
    # We vendor lowdown and include it as "lowdown/lowdown.h" from ${THIRD_PARTY_DIR}.
    # To avoid header/library version mismatches (common with system lowdown packages),
    # always build from the vendored sources or use a prebuilt vendored archive.
    set(LOWDOWN_DIR ${THIRD_PARTY_DIR}/lowdown)

    if(EXISTS ${LOWDOWN_DIR}/liblowdown.a)
        message(STATUS "Using prebuilt vendored lowdown: ${LOWDOWN_DIR}/liblowdown.a")
        add_library(lowdown STATIC IMPORTED)
        set_target_properties(lowdown PROPERTIES
            IMPORTED_LOCATION ${LOWDOWN_DIR}/liblowdown.a
            INTERFACE_INCLUDE_DIRECTORIES ${THIRD_PARTY_DIR}
        )
        target_link_libraries(lowdown INTERFACE m)
    else()
        message(STATUS "Building vendored lowdown from source: ${LOWDOWN_DIR}")
        add_library(lowdown STATIC
            ${LOWDOWN_DIR}/autolink.c
            ${LOWDOWN_DIR}/buffer.c
            ${LOWDOWN_DIR}/compats.c
            ${LOWDOWN_DIR}/diff.c
            ${LOWDOWN_DIR}/document.c
            ${LOWDOWN_DIR}/entity.c
            ${LOWDOWN_DIR}/gemini.c
            ${LOWDOWN_DIR}/gemini_escape.c
            ${LOWDOWN_DIR}/html.c
            ${LOWDOWN_DIR}/html_escape.c
            ${LOWDOWN_DIR}/latex.c
            ${LOWDOWN_DIR}/latex_escape.c
            ${LOWDOWN_DIR}/library.c
            ${LOWDOWN_DIR}/libdiff.c
            ${LOWDOWN_DIR}/odt.c
            ${LOWDOWN_DIR}/roff.c
            ${LOWDOWN_DIR}/roff_escape.c
            ${LOWDOWN_DIR}/roff_manpage.c
            ${LOWDOWN_DIR}/smartypants.c
            ${LOWDOWN_DIR}/template.c
            ${LOWDOWN_DIR}/term.c
            ${LOWDOWN_DIR}/tree.c
            ${LOWDOWN_DIR}/util.c
        )
        target_include_directories(lowdown PUBLIC
            ${THIRD_PARTY_DIR}
            ${LOWDOWN_DIR}
        )
        target_link_libraries(lowdown PUBLIC m)
    endif()
endif()

