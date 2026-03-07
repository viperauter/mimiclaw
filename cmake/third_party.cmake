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