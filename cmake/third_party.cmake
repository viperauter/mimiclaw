include(FetchContent)

set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/third_party)

# =====================================================
# mongoose
# =====================================================

if(NOT EXISTS ${THIRD_PARTY_DIR}/mongoose)
    message(STATUS "Fetching mongoose from GitHub...")
else()
    message(STATUS "Using cached mongoose source: ${THIRD_PARTY_DIR}/mongoose")
endif()

FetchContent_Declare(
    mongoose
    GIT_REPOSITORY https://gitee.com/mirrors/mongoose.git
    GIT_TAG 7.14
    SOURCE_DIR ${THIRD_PARTY_DIR}/mongoose
)

FetchContent_Populate(mongoose)

add_library(mongoose STATIC
    ${mongoose_SOURCE_DIR}/mongoose.c
)

target_include_directories(mongoose PUBLIC
    ${mongoose_SOURCE_DIR})

# =====================================================
# cJSON
# =====================================================

if(NOT EXISTS ${THIRD_PARTY_DIR}/cjson)
    message(STATUS "Fetching cJSON from GitHub...")
else()
    message(STATUS "Using cached cJSON source: ${THIRD_PARTY_DIR}/cjson")
endif()

FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG v1.7.18
    SOURCE_DIR ${THIRD_PARTY_DIR}/cjson
)

FetchContent_Populate(cjson)

add_library(cjson STATIC
    ${cjson_SOURCE_DIR}/cJSON.c
)

target_include_directories(cjson PUBLIC
    ${cjson_SOURCE_DIR})

# =====================================================
# nanopb - Protocol Buffers for Embedded Systems
# =====================================================

if(NOT EXISTS ${THIRD_PARTY_DIR}/nanopb)
    message(STATUS "Fetching nanopb from GitHub...")
else()
    message(STATUS "Using cached nanopb source: ${THIRD_PARTY_DIR}/nanopb")
endif()

FetchContent_Declare(
    nanopb
    GIT_REPOSITORY https://github.com/nanopb/nanopb.git
    GIT_TAG 0.4.9.1
    SOURCE_DIR ${THIRD_PARTY_DIR}/nanopb
)

FetchContent_Populate(nanopb)

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

add_custom_command(
    OUTPUT ${FEISHU_PB_HEADER} ${FEISHU_PB_SOURCE}
    COMMAND ${Python3_EXECUTABLE}
        ${nanopb_SOURCE_DIR}/generator/nanopb_generator.py
        -I ${FEISHU_PB_DIR}
        ${FEISHU_PB_PROTO}
    DEPENDS ${FEISHU_PB_PROTO}
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
