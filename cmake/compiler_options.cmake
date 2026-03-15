add_compile_options(
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# TLS backend configuration
if(TLS_BACKEND STREQUAL "builtin")
    add_compile_definitions(
        MG_TLS=MG_TLS_BUILTIN
    )
elseif(TLS_BACKEND STREQUAL "openssl")
    add_compile_definitions(
        MG_TLS=MG_TLS_OPENSSL
    )
else()
    # Default fallback
    if(WIN32)
        add_compile_definitions(
            MG_TLS=MG_TLS_BUILTIN
        )
    else()
        add_compile_definitions(
            MG_TLS=MG_TLS_OPENSSL
        )
    endif()
endif()

if(WIN32)
    add_compile_definitions(
        _WIN32_WINNT=0x0601
    )
    add_compile_options(
        -D_POSIX_C_SOURCE=200809L
    )
endif()
