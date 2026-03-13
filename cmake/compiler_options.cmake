add_compile_options(
    -Wall
    -Wextra
    -Wno-unused-parameter
)

if(WIN32)
    add_compile_definitions(
        MG_TLS=MG_TLS_BUILTIN
        _WIN32_WINNT=0x0601
    )
    add_compile_options(
        -D_POSIX_C_SOURCE=200809L
    )
else()
    # On non-Windows (POSIX) use OpenSSL-backed TLS for Mongoose
    add_compile_definitions(
        MG_TLS=MG_TLS_OPENSSL
    )
endif()